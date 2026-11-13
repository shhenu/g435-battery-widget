# Third-party notices

The 046D:0ADF HID framing, CRC-16/XMODEM transport, session initialization,
remote-mode commands, battery response layout, and voltage-to-percentage curve
were adapted from the MIT-licensed research and implementation in
[portyporty/g435-battery-percentage](https://github.com/portyporty/g435-battery-percentage),
by Oğuz Kayra Bozkurt. This project reimplements that transport in C / Win32.

Reference: [TrayApp.cs, commit 2784e1b](https://github.com/portyporty/g435-battery-percentage/blob/2784e1b3269654cdaea6dcc8caef4cfff9152083/src/TrayApp.cs).

The upstream license is reproduced below.

The 046D:0ACB Realtek bridge is independently implemented from the documented
PA/PI protocol and supplied packet captures. Additional protocol cross-checks:
[deddroid/logitech-g435-battery](https://github.com/deddroid/logitech-g435-battery).
No Rust implementation from that project is included in this repository.

---

MIT License

Copyright (c) 2026 Oğuz Kayra Bozkurt

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
