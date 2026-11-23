/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "FlexPhoneKeyboard.h"

#include "FlexQr.h"
#include "ServiceBroker.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "guilib/GUIMessage.h"
#include "messaging/ApplicationMessenger.h"
#include "network/Network.h"
#include "pictures/Picture.h"
#include "utils/log.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
constexpr uint16_t PORT = 8781; // 8770 browser service, 8780 flexapi (docs/control-api)
constexpr int CTL_EDIT = 312; // GUIDialogKeyboardGeneric.cpp
constexpr size_t MAX_BODY = 1024;
constexpr size_t MAX_TEXT = 256;
constexpr size_t TOKEN_LEN = 24;
constexpr int MODULE_PX = 8;
constexpr int QUIET_MODULES = 4;
constexpr auto PENDING_TIME = std::chrono::seconds(10);
constexpr const char* OFF_FILE = "/APPS/kodi-phonekbd.off";
constexpr const char* TOKEN_FILE = "special://masterprofile/flexkbd.token";

// The page on the phone. It asks the TV every second (every 2 s while hidden) whether a keyboard
// dialog is open. Open: every change is sent as the whole text (the dialog takes it as a whole),
// "Fertig" also confirms. Closed: "Suchen" starts the library search with the text.
// Same origin only, no external resources; the source stays ASCII (umlauts as escapes).
const char PAGE[] = R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Kodi-Tastatur</title>
<style>
body{font-family:system-ui,sans-serif;background:#10121c;color:#eee;margin:0;padding:16px}
h1{font-size:18px;font-weight:600;margin:0 0 4px}
#m{font-size:14px;color:#aaa;margin:0 0 12px;min-height:1.2em}
input{width:100%;box-sizing:border-box;font-size:20px;padding:12px;border-radius:8px;border:1px solid #555;background:#1c1f2e;color:#fff}
.row{display:flex;gap:8px;margin-top:12px}
button{flex:1;font-size:18px;padding:12px;border:0;border-radius:8px;background:#3a6df0;color:#fff}
button.sec{background:#444}
#st{margin-top:10px;font-size:14px;color:#aaa;min-height:1.2em}
</style></head>
<body><h1 id="h">Kodi-Suche</h1><div id="m"></div>
<input id="t" autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false" enterkeyhint="search">
<div class="row"><button class="sec" id="c">Leeren</button><button id="ok">Suchen</button></div>
<div id="st"></div>
<script>
function $(i){return document.getElementById(i)}
var base=location.pathname.replace(/\/$/,''),t=$('t'),st=$('st'),ok=$('ok'),h=$('h'),m=$('m');
var open=false,session=-1,busy=false,again=false,timer=0;
function say(s){st.textContent=s}
function req(meth,p,v,cb){var x=new XMLHttpRequest();x.open(meth,base+p);x.timeout=8000;
if(meth=='POST')x.setRequestHeader('Content-Type','text/plain;charset=utf-8');
x.onload=function(){cb(x.status,x.responseText)};x.onerror=x.ontimeout=function(){cb(0,'')};x.send(v)}
function show(){ok.textContent=open?'Fertig':'Suchen';t.enterKeyHint=open?'done':'search';
m.textContent=open?'Eingabefeld am TV ist offen: der Text erscheint dort direkt.':'Begriff eingeben und \u201eSuchen\u201c tippen: der TV sucht in der Bibliothek.'}
function push(){if(!open)return;if(busy){again=true;return}busy=true;
req('POST','/t',t.value,function(){busy=false;if(again){again=false;push()}})}
function fail(s){say(s==0||s==503?'Kodi nicht erreichbar (l\u00e4uft Kodi?).':s==404?'Code ung\u00fcltig: QR-Code am TV neu scannen.':
s==409?'Das Eingabefeld am TV ist schon zu.':'Fehler '+s)}
t.addEventListener('input',push);
$('c').onclick=function(){t.value='';push();t.focus()};
ok.onclick=function(){var v=t.value.trim(),wasOpen=open;
req('POST',open?'/ok':'/search',t.value,function(s){if(s!=200){fail(s);return}
say(wasOpen?'Gesendet.':v?'Suche nach \u201e'+v+'\u201c l\u00e4uft am TV.':'Suche am TV ge\u00f6ffnet.');t.value='';t.blur();poll()})};
t.addEventListener('keydown',function(e){if(e.key==='Enter'){e.preventDefault();ok.click()}});
function poll(){clearTimeout(timer);
req('GET','/s',null,function(s,b){
if(s==200){try{var o=JSON.parse(b);if(st.textContent.indexOf('Kodi nicht')==0)say('');
if(o.open!==open||o.session!==session){var fresh=o.open&&o.session!==session;open=o.open;session=o.session;
h.textContent=open&&o.heading?o.heading:'Kodi-Suche';if(fresh)t.value=o.text||'';show()}}catch(e){}}
else fail(s);
timer=setTimeout(poll,document.hidden?2000:1000)})}
document.addEventListener('visibilitychange',function(){if(!document.hidden)poll()});
show();poll();
</script></body></html>
)HTML";

bool Disabled()
{
  const char* env = std::getenv("FLEX_KODI_PHONEKBD");
  if (env && std::strcmp(env, "0") == 0)
    return true;
  return access(OFF_FILE, F_OK) == 0;
}

bool ValidToken(const std::string& token)
{
  if (token.size() != TOKEN_LEN)
    return false;
  for (const char c : token)
  {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

//! the token of userdata/flexkbd.token, a new one (96 bits from the kernel) if there is none
std::string LoadToken()
{
  const std::string path = CSpecialProtocol::TranslatePath(TOKEN_FILE);
  {
    std::ifstream in(path);
    std::string token;
    if (in && std::getline(in, token) && ValidToken(token))
      return token;
  }
  static const char hex[] = "0123456789abcdef";
  std::random_device rd;
  std::string token;
  for (size_t i = 0; i < TOKEN_LEN; i++)
    token += hex[rd() & 15];
  const mode_t old = umask(077);
  {
    std::ofstream out(path, std::ios::trunc);
    out << token << '\n';
    if (!out)
      CLog::Log(LOGWARNING, "CFlexPhoneKeyboard: token file cannot be written, new code each start");
  }
  umask(old);
  chmod(path.c_str(), 0600);
  return token;
}

bool HomeNetwork(const sockaddr* addr)
{
  if (!addr || addr->sa_family != AF_INET)
    return false; // IPv6 is not filtered on the TV (no ip6tables): IPv4 only
  const uint32_t ip = ntohl(reinterpret_cast<const sockaddr_in*>(addr)->sin_addr.s_addr);
  return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 24) == 127;
}

std::string JsonEscape(const std::string& s)
{
  std::string out;
  for (const char c : s)
  {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\')
    {
      out += '\\';
      out += c;
    }
    else if (u < 0x20)
    {
      static const char hex[] = "0123456789abcdef";
      out += "\\u00";
      out += hex[u >> 4];
      out += hex[u & 15];
    }
    else
      out += c;
  }
  return out;
}

//! one line of at most MAX_TEXT bytes, cut at a character boundary
std::string CleanText(const std::string& in)
{
  std::string out;
  for (const char c : in)
  {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '\r' || c == '\n' || c == '\t')
      out += ' ';
    else if (u >= 0x20 && u != 0x7F)
      out += c;
  }
  if (out.size() > MAX_TEXT)
  {
    size_t cut = MAX_TEXT;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
      cut--; // not inside a UTF-8 sequence
    out.resize(cut);
  }
  return out;
}

bool WriteQrImage(const std::string& url, const std::string& file)
{
  FLEXQR::Code code;
  if (!FLEXQR::Encode(url, code))
    return false;
  const int px = (code.size + 2 * QUIET_MODULES) * MODULE_PX;
  std::vector<unsigned char> image(static_cast<size_t>(px) * static_cast<size_t>(px) * 4, 255);
  for (int y = 0; y < code.size; y++)
  {
    for (int x = 0; x < code.size; x++)
    {
      if (!code.Dark(x, y))
        continue;
      for (int dy = 0; dy < MODULE_PX; dy++)
      {
        const size_t row = static_cast<size_t>((y + QUIET_MODULES) * MODULE_PX + dy);
        unsigned char* p =
            image.data() + (row * static_cast<size_t>(px) +
                            static_cast<size_t>((x + QUIET_MODULES) * MODULE_PX)) * 4;
        for (int dx = 0; dx < MODULE_PX; dx++, p += 4)
          p[0] = p[1] = p[2] = 0; // BGRA, alpha stays 255
      }
    }
  }
  return CPicture::CreateThumbnailFromSurface(image.data(), px, px, px * 4, file);
}

MHD_Result Reply(MHD_Connection* c, unsigned int status, const char* type, const std::string& body)
{
  MHD_Response* r = MHD_create_response_from_buffer(body.size(), const_cast<char*>(body.data()),
                                                    MHD_RESPMEM_MUST_COPY);
  if (!r)
    return MHD_NO;
  MHD_add_response_header(r, "Content-Type", type);
  MHD_add_response_header(r, "Cache-Control", "no-store");
  MHD_add_response_header(r, "X-Content-Type-Options", "nosniff");
  MHD_add_response_header(r, "X-Frame-Options", "DENY");
  MHD_add_response_header(r, "Referrer-Policy", "no-referrer");
  const MHD_Result ret = MHD_queue_response(c, status, r);
  MHD_destroy_response(r);
  return ret;
}

struct Upload
{
  std::string body;
  bool tooLong = false;
};
} // namespace

struct CFlexPhoneKeyboardServer
{
  static MHD_Result Accept(void*, const sockaddr* addr, socklen_t)
  {
    return HomeNetwork(addr) ? MHD_YES : MHD_NO;
  }

  static MHD_Result Handle(void* cls,
                           MHD_Connection* c,
                           const char* url,
                           const char* method,
                           const char*,
                           const char* data,
                           size_t* size,
                           void** conCls)
  {
    auto* self = static_cast<CFlexPhoneKeyboard*>(cls);
    const bool post = std::strcmp(method, "POST") == 0;
    if (!post && std::strcmp(method, "GET") != 0)
      return Reply(c, 405, "text/plain", "method not allowed");
    std::string body;
    if (post)
    {
      if (!*conCls)
      {
        *conCls = new Upload();
        return MHD_YES;
      }
      Upload* upload = static_cast<Upload*>(*conCls);
      if (*size)
      {
        if (upload->body.size() + *size <= MAX_BODY)
          upload->body.append(data, *size);
        else
          upload->tooLong = true;
        *size = 0;
        return MHD_YES;
      }
      if (upload->tooLong)
        return Reply(c, 413, "text/plain", "too long");
      body = upload->body;
    }
    return self->Answer(c, url ? url : "", post, body) ? MHD_YES : MHD_NO;
  }

  static void Completed(void*, MHD_Connection*, void** conCls, MHD_RequestTerminationCode)
  {
    delete static_cast<Upload*>(*conCls);
    *conCls = nullptr;
  }
};

CFlexPhoneKeyboard& CFlexPhoneKeyboard::GetInstance()
{
  static CFlexPhoneKeyboard instance;
  return instance;
}

void CFlexPhoneKeyboard::Startup()
{
  if (!Disabled())
    EnsureServer();
}

bool CFlexPhoneKeyboard::EnsureServer()
{
  std::string token;
  {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_daemon)
      return true;
    if (m_closing)
      return false;
    if (m_token.empty())
      m_token = LoadToken();
    token = m_token;
  }
  MHD_Daemon* daemon = MHD_start_daemon(
      MHD_USE_INTERNAL_POLLING_THREAD, PORT, &CFlexPhoneKeyboardServer::Accept, nullptr,
      &CFlexPhoneKeyboardServer::Handle, this, MHD_OPTION_CONNECTION_LIMIT, 8u,
      MHD_OPTION_PER_IP_CONNECTION_LIMIT, 4u, MHD_OPTION_CONNECTION_TIMEOUT, 15u,
      MHD_OPTION_CONNECTION_MEMORY_LIMIT, static_cast<size_t>(32 * 1024),
      MHD_OPTION_NOTIFY_COMPLETED, &CFlexPhoneKeyboardServer::Completed, nullptr, MHD_OPTION_END);
  if (!daemon)
  {
    CLog::Log(LOGERROR, "CFlexPhoneKeyboard: port {} cannot be opened, no phone keyboard", PORT);
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(m_lock);
    m_daemon = daemon;
  }
  CLog::Log(LOGINFO, "CFlexPhoneKeyboard: phone keyboard on port {}", PORT);
  return true;
}

std::string CFlexPhoneKeyboard::EnsureImage(const std::string& url)
{
  if (url == m_url && !m_imageFile.empty() && XFILE::CFile::Exists(m_imageFile))
    return m_imageFile;
  // a new name for a new code: the texture manager keeps images by path
  const std::string image = "special://temp/flexkbd-qr-" + std::to_string(++m_imageCount) + ".png";
  if (!WriteQrImage(url, image))
  {
    CLog::Log(LOGERROR, "CFlexPhoneKeyboard: QR code image cannot be written");
    return "";
  }
  if (!m_imageFile.empty())
    XFILE::CFile::Delete(m_imageFile);
  m_url = url;
  m_imageFile = image;
  return image;
}

void CFlexPhoneKeyboard::Shutdown()
{
  MHD_Daemon* daemon = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_lock);
    m_closing = true; // requests still on the way get 503 from here on
    std::swap(daemon, m_daemon);
    m_dialogId = 0;
  }
  if (daemon)
  {
    MHD_stop_daemon(daemon); // waits for running handlers; they never wait for the GUI thread
    CLog::Log(LOGINFO, "CFlexPhoneKeyboard: phone keyboard off");
  }
  if (!m_imageFile.empty())
    XFILE::CFile::Delete(m_imageFile);
  m_imageFile.clear();
  m_url.clear();
}

std::string CFlexPhoneKeyboard::DialogOpened(int dialogId,
                                             const std::string& heading,
                                             const std::string& text)
{
  if (Disabled() || !EnsureServer())
    return "";
  std::string pending;
  std::string token;
  {
    std::lock_guard<std::mutex> lock(m_lock);
    m_dialogId = dialogId;
    m_heading = heading;
    m_text = text;
    m_session++;
    if (!m_pendingText.empty() && std::chrono::steady_clock::now() < m_pendingUntil)
      pending = m_pendingText;
    m_pendingText.clear();
    token = m_token;
  }
  if (!pending.empty())
    SendText(pending, true); // "Suchen" on the phone: this dialog is the search it started

  CNetworkInterface* iface = CServiceBroker::GetNetwork().GetFirstConnectedInterface();
  const std::string ip = iface ? iface->GetCurrentIPAddress() : "";
  if (ip.empty() || ip.find(':') != std::string::npos || ip == "0.0.0.0")
  {
    CLog::Log(LOGINFO, "CFlexPhoneKeyboard: no IPv4 address, no QR code");
    return "";
  }
  return EnsureImage("http://" + ip + ":" + std::to_string(PORT) + "/k/" + token);
}

void CFlexPhoneKeyboard::DialogClosed()
{
  std::lock_guard<std::mutex> lock(m_lock);
  m_dialogId = 0;
  m_heading.clear();
  m_text.clear();
}

int CFlexPhoneKeyboard::Answer(MHD_Connection* c, const std::string& path, bool post, const std::string& body)
{
  std::string token;
  std::string heading;
  std::string text;
  bool open = false;
  unsigned int session = 0;
  {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_closing)
      return Reply(c, 503, "text/plain", "Kodi is closing");
    token = m_token;
    heading = m_heading;
    text = m_text;
    open = m_dialogId != 0;
    session = m_session;
  }
  const std::string prefix = "/k/" + token;
  if (token.empty() || path.compare(0, prefix.size(), prefix) != 0)
    return Reply(c, 404, "text/plain", "not found");
  const std::string rest = path.substr(prefix.size());
  if (!post && (rest.empty() || rest == "/"))
    return Reply(c, 200, "text/html; charset=utf-8", PAGE);
  if (!post && rest == "/s")
    return Reply(c, 200, "application/json; charset=utf-8",
                 std::string("{\"open\":") + (open ? "true" : "false") +
                     ",\"session\":" + std::to_string(session) + ",\"heading\":\"" +
                     JsonEscape(heading) + "\",\"text\":\"" + JsonEscape(text) + "\"}");
  if (post && (rest == "/t" || rest == "/ok"))
  {
    if (!open)
      return Reply(c, 409, "text/plain", "no keyboard open");
    SendText(CleanText(body), rest == "/ok");
    return Reply(c, 200, "text/plain", "ok");
  }
  if (post && rest == "/search")
  {
    if (open) // opened on the TV meanwhile: the text goes there
      SendText(CleanText(body), true);
    else
      StartSearch(CleanText(body));
    return Reply(c, 200, "text/plain", "ok");
  }
  return Reply(c, 404, "text/plain", "not found");
}

void CFlexPhoneKeyboard::SendText(const std::string& text, bool done)
{
  int dialogId = 0;
  {
    std::lock_guard<std::mutex> lock(m_lock);
    dialogId = m_dialogId;
    m_text = text; // a reloaded page shows it again
  }
  if (!dialogId)
    return;
  // like the speech recognition of this dialog: to the GUI thread, the dialog sets the edit
  // control; param1 = 1 confirms it (OnOK)
  CGUIMessage msg(GUI_MSG_SET_TEXT, dialogId, CTL_EDIT, done ? 1 : 0);
  msg.SetLabel(text);
  CServiceBroker::GetAppMessenger()->SendGUIMessage(msg, dialogId);
}

void CFlexPhoneKeyboard::StartSearch(const std::string& text)
{
  {
    std::lock_guard<std::mutex> lock(m_lock);
    m_pendingText = text; // empty: the search only opens, the text is typed live
    m_pendingUntil = std::chrono::steady_clock::now() + PENDING_TIME;
  }
  CLog::Log(LOGINFO, "CFlexPhoneKeyboard: library search from the phone");
  // as "Search local library" in Estuary's search dialog (Custom_1107_SearchDialog.xml); the
  // messages run one after the other in the application thread, never waited for here
  const auto messenger = CServiceBroker::GetAppMessenger();
  if (!messenger)
    return;
  messenger->PostMsg(TMSG_EXECUTE_BUILT_IN, -1, -1, nullptr, "Dialog.Close(all)");
  messenger->PostMsg(TMSG_EXECUTE_BUILT_IN, -1, -1, nullptr,
                     "ActivateWindow(Videos,videodb://movies/titles/,return)");
  messenger->PostMsg(TMSG_EXECUTE_BUILT_IN, -1, -1, nullptr, "VideoLibrary.Search");
}
