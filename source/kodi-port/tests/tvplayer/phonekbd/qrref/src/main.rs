// Vergleichsmatrix mit dem Crate qrcode: gleicher Bitstrom wie FlexQr (nur Byte-Modus).
// Aufruf: qrref <version> <L|M> <text>
use qrcode::bits::Bits;
use qrcode::{EcLevel, QrCode, Version};

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let ver: i16 = args[1].parse().unwrap();
    let ecl = if args[2] == "L" { EcLevel::L } else { EcLevel::M };
    let mut bits = Bits::new(Version::Normal(ver));
    bits.push_byte_data(args[3].as_bytes()).unwrap();
    bits.push_terminator(ecl).unwrap();
    let code = QrCode::with_bits(bits, ecl).unwrap();
    let w = code.width();
    let colors = code.to_colors();
    for y in 0..w {
        let row: String = (0..w).map(|x| if colors[y * w + x] == qrcode::Color::Dark { '#' } else { '.' }).collect();
        println!("{}", row);
    }
}
