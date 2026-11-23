#!/bin/bash
# Teil A: Funktionen des Wächters einzeln (islink_to, ind, rtdirs_ok) und rndhex/flexkey aus kodi.sh
T=$1
export PATH=$T/bb:$PATH
cd "$T"
W=APPS/kodi/kodi-watch.sh
sed -n '1,/^mkdir -m 700 "\$R"/p' $W | sed '$d' > out/wfuncs.sh
sed '/^case "\$1" in/,$d' APPS/kodi/kodi.sh > out/kfuncs.sh
mkdir -p tmp/flexkodi.d && chmod 700 tmp/flexkodi.d
cat > out/a.sh <<'EOF'
. ./out/wfuncs.sh
mkdir -p "$T/lt" && cd "$T/lt" || exit 1
want=$RT/b
chk() { if islink_to rt "$want"; then r=JA; else r=nein; fi; echo "$1: $r (soll $2)"; }
rm -f rt; ln -s "$want" rt; chk "richtig" JA
rm -f rt; ln -s "$want/" rt; chk "mit Schrägstrich" nein
rm -f rt; ln -s "x -> $want" rt; chk "Ziel mit ->" nein
rm -f rt; ln -s "$(printf 'a\n%s' "$want")" rt; chk "Ziel mit Zeilenumbruch" nein
rm -f rt; ln -s "$(dirname "$want")/./b" rt; chk "anderer Weg" nein
rm -f rt; echo x > rt; chk "Datei" nein
rm -f rt; mkdir rt; chk "Ordner" nein; rmdir rt
rm -f rt; chk "fehlt" nein
ln -s "$want" "rt x"; if islink_to "rt x" "$want"; then echo "Name mit Leerzeichen: JA"; fi; rm -f "rt x"
EOF
T=$T busybox ash -c "T=$T; . ./out/a.sh"
echo "--- rndhex (kodi.sh)"
busybox ash -c ". ./out/kfuncs.sh; a=\$(rndhex 16); b=\$(rndhex 8); echo \"\${#a} \${#b}\"; case \$a\$b in *[!0-9a-f]*) echo FALSCH;; *) echo nur-hex;; esac; [ \"\$a\" != \"\$(rndhex 16)\" ] && echo verschieden"
echo "--- rndhex Rückfall ohne hexdump"
mkdir -p out/nohex; for a in head md5sum cut; do ln -sf /usr/bin/busybox out/nohex/$a; done
PATH=$T/out/nohex:/usr/bin/nonexistent busybox ash -c ". ./out/kfuncs.sh; a=\$(rndhex 16); b=\$(rndhex 8); echo \"\${#a} \${#b} \$(echo \$a | busybox cut -c1-4)…\"" 2>&1
echo "--- hexdump-Format"
busybox hexdump -v -n 4 -e '1/1 "%02x"' /dev/zero; echo
echo "--- noclobber-Verhalten von ash (Begründung für flexkey/Zufallsnamen)"
mkdir -p out/nc && cd out/nc && rm -f f1 f2 fifo && mkfifo fifo && ln -s /dev/null f1 && ln -s fifo f2 2>/dev/null
busybox ash -c 'set -C; echo x > f1 && echo "Symlink auf Gerät /dev/null: geschrieben (kein O_EXCL)"' 2>&1
busybox ash -c 'set -C; echo x > f3 && echo "neuer Name: angelegt"; echo y > f3 || echo "zweites Mal: abgelehnt"' 2>&1
ln -s nirgends f4; busybox ash -c 'set -C; echo x > f4 && echo "hängender Symlink: geschrieben" || echo "hängender Symlink: abgelehnt"' 2>&1
cd "$T"
echo "--- ln -n auf belegte Namen"
mkdir -p out/ln && cd out/ln && rm -rf * && echo k > src && mkdir d && ln -s d sd && ln -s /etc/hostname sf
busybox ln -n src sd 2>&1; ls d
busybox ln -n src sf 2>&1
busybox ln -n src neu && ls -ln neu | cut -c1-20
busybox ln src sd 2>&1; echo "ohne -n in Symlink auf Ordner:"; ls d
cd "$T"
echo UNIT_DONE
