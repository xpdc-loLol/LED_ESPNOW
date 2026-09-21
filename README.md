Penerangan Projek
Projek ini menggunakan ESP32-S3 untuk mengawal satu LED RGB WS2812 melalui laman web.

Selepas ESP32-S3 berjaya menyambung ke Wi-Fi:

ESP32-S3 menjalankan web server.
Pengguna membuka alamat IP ESP32 melalui browser.
Pengguna boleh:
Menetapkan warna merah, hijau, biru, putih atau OFF.
Memilih warna tersuai.
Mengubah brightness.
OLED I2C memaparkan:
Status Wi-Fi.
Alamat IP.
Warna semasa.
Nilai RGB.
ESP32-S3 turut mencetak maklumat hardware semasa startup seperti flash, PSRAM dan heap.

Sambungan Hardware
Komponen	ESP32-S3
WS2812 data	GPIO 48
OLED SDA	GPIO 8
OLED SCL	GPIO 9
OLED I2C address	0x3C
OLED I2C speed	400 kHz

Konfigurasi Wi-Fi
Jalankan: idf.py menuconfig
