# Generates firmware_v3/docs/keypad-wiring.svg (run from anywhere).
W, H = 1100, 740
o = []
a = o.append
a(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" font-family="Helvetica, Arial, sans-serif">')
a(f'<rect width="{W}" height="{H}" fill="#ffffff"/>')
a('<text x="30" y="34" font-size="22" font-weight="bold" fill="#111">FloodMesh V3 - 4x4 membrane keypad to Heltec WiFi LoRa 32 V3</text>')
a('<text x="30" y="56" font-size="13" fill="#555">8 wires, no resistors. Every keypad wire goes to header J3. Row order is reversed (keypad pin 1 goes to J3-10); columns run in order.</text>')

ROWC = ["#d62728", "#ff7f0e", "#b8860b", "#8c564b"]   # R1..R4
COLC = ["#1f77b4", "#17becf", "#2ca02c", "#9467bd"]   # C1..C4

# ---- keypad
kx, ky, ks, kg = 70, 90, 50, 8
labels = [["1","2","3","A"],["4","5","6","B"],["7","8","9","C"],["*","0","#","D"]]
a(f'<rect x="{kx-18}" y="{ky-14}" width="{4*ks+3*kg+36}" height="{4*ks+3*kg+28}" rx="10" fill="#f3f3f3" stroke="#333" stroke-width="2"/>')
for r in range(4):
    for c in range(4):
        x, y = kx + c*(ks+kg), ky + r*(ks+kg)
        fill = "#fde2e2" if c == 3 else "#ffffff"
        a(f'<rect x="{x}" y="{y}" width="{ks}" height="{ks}" rx="6" fill="{fill}" stroke="#666"/>')
        a(f'<text x="{x+ks/2}" y="{y+ks/2+8}" font-size="22" text-anchor="middle" fill="#111">{labels[r][c]}</text>')
    y = ky + r*(ks+kg) + ks/2 + 5
    a(f'<text x="{kx-26}" y="{y}" font-size="13" font-weight="bold" text-anchor="end" fill="{ROWC[r]}">R{r+1}</text>')
for c in range(4):
    x = kx + c*(ks+kg) + ks/2
    a(f'<text x="{x}" y="{ky-20}" font-size="13" font-weight="bold" text-anchor="middle" fill="{COLC[c]}">C{c+1}</text>')
a(f'<text x="{kx+4*ks+3*kg+30}" y="{ky+20}" font-size="12" fill="#555">keypad face up,</text>')
a(f'<text x="{kx+4*ks+3*kg+30}" y="{ky+36}" font-size="12" fill="#555">ribbon hanging down</text>')
a(f'<text x="{kx+4*ks+3*kg+30}" y="{ky+60}" font-size="12" fill="#555">A = up   B = down</text>')
a(f'<text x="{kx+4*ks+3*kg+30}" y="{ky+76}" font-size="12" fill="#555">C = OK   D = back</text>')
a(f'<text x="{kx+4*ks+3*kg+30}" y="{ky+100}" font-size="12" fill="#555">SOS = hold * and # 3 s</text>')

# ---- ribbon + connector (pins 1..8 left to right)
cy = 372
cx = [80 + i*28 for i in range(8)]
kb = ky + 4*ks + 3*kg + 14
a(f'<path d="M{cx[0]-8},{kb} L{cx[7]+8},{kb} L{cx[7]+8},{cy-14} L{cx[0]-8},{cy-14} Z" fill="#e8e0c8" stroke="#999"/>')
for i in range(8):
    a(f'<line x1="{cx[i]}" y1="{kb}" x2="{cx[i]}" y2="{cy-14}" stroke="#b0a070" stroke-width="2"/>')
a(f'<text x="{cx[7]+16}" y="{(kb+cy)/2}" font-size="12" fill="#555">ribbon</text>')
a(f'<rect x="{cx[0]-14}" y="{cy-14}" width="{cx[7]-cx[0]+28}" height="22" fill="#222" rx="3"/>')
for i in range(8):
    a(f'<rect x="{cx[i]-5}" y="{cy-9}" width="10" height="10" fill="#bbb"/>')
    a(f'<text x="{cx[i]}" y="{cy-20-(kb>0)*0}" font-size="0"></text>')
a(f'<text x="{cx[7]+22}" y="{cy+2}" font-size="12" fill="#555">keypad connector (female, 2.54 mm)</text>')
a(f'<text x="{cx[0]-20}" y="{cy+2}" font-size="12" text-anchor="end" fill="#111">pin</text>')

# ---- Heltec J3
jy = 560
jx = lambda n: 95 + (n-1)*33
a(f'<rect x="30" y="{jy-22}" width="{jx(18)+10}" height="118" rx="10" fill="#1d3b5a" stroke="#0d2338" stroke-width="2"/>')
a(f'<text x="{(jx(18)+40)/2}" y="{jy+70}" font-size="15" fill="#fff" text-anchor="middle">Heltec WiFi LoRa 32 V3  -  header J3</text>')
a(f'<text x="{(jx(18)+40)/2}" y="{jy+87}" font-size="11" fill="#cfd8e3" text-anchor="middle">count pins from the GND / 3V3 / 3V3 end of J3</text>')
gp = {1:"GND",2:"3V3",3:"3V3",4:"37",5:"46",6:"45",7:"42",8:"41",9:"40",10:"39",11:"38",12:"1",13:"2",14:"3",15:"4",16:"5",17:"6",18:"7"}
avoid = {4,5,6,14}
used = {7,8,9,10,13,15,16,17,18}
for n in range(1,19):
    x = jx(n)
    col = "#ffd700" if n in used else ("#ff6b6b" if n in avoid else "#9aa7b4")
    a(f'<rect x="{x-7}" y="{jy-7}" width="14" height="14" fill="{col}" stroke="#000"/>')
    a(f'<text x="{x}" y="{jy+26}" font-size="11" fill="#fff" text-anchor="middle">{n}</text>')
    a(f'<text x="{x}" y="{jy+44}" font-size="11" fill="{"#ffd700" if n in used else "#cfd8e3"}" text-anchor="middle">{gp[n]}</text>')
a(f'<text x="36" y="{jy+26}" font-size="10" fill="#cfd8e3" text-anchor="start"></text>')
a(f'<text x="{jx(1)-24}" y="{jy+26}" font-size="10" fill="#cfd8e3" text-anchor="end">J3-</text>')
a(f'<text x="{jx(1)-24}" y="{jy+44}" font-size="10" fill="#cfd8e3" text-anchor="end">GPIO</text>')

# ---- wires
def wire(x1, y1, lane, x2, y2, color, dash=False):
    d = f'M{x1},{y1+2} L{x1},{lane} L{x2},{lane} L{x2},{y2-7}'
    a(f'<path d="{d}" fill="none" stroke="#fff" stroke-width="7" stroke-linejoin="round"/>')
    a(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="3.5" stroke-linejoin="round"/>')
    a(f'<circle cx="{x2}" cy="{y2-7}" r="4" fill="{color}"/>')
    a(f'<circle cx="{x1}" cy="{y1+2}" r="4" fill="{color}"/>')

# columns first (under), nested lanes: pin8 highest
col_dst = [15,16,17,18]
for i in range(3, -1, -1):
    wire(cx[4+i], cy, 395 + (3-i)*12, jx(col_dst[i]), jy, COLC[i])
# rows: pin k -> J3-(11-k); lanes below the column lanes
row_dst = [10,9,8,7]
for i in range(4):
    wire(cx[i], cy, 455 + i*14, jx(row_dst[i]), jy, ROWC[i])

# connector pin labels under the connector
names = ["R1","R2","R3","R4","C1","C2","C3","C4"]
cols = ROWC + COLC
for i in range(8):
    a(f'<text x="{cx[i]}" y="{cy+24}" font-size="12" font-weight="bold" fill="#111" text-anchor="middle" stroke="#fff" stroke-width="3" paint-order="stroke">{i+1}</text>')

# buzzer stub on J3-13
bx = jx(13)
a(f'<line x1="{bx}" y1="{jy-7}" x2="{bx}" y2="{jy-60}" stroke="#444" stroke-width="3" stroke-dasharray="6,4"/>')
a(f'<text x="{bx+6}" y="{jy-64}" font-size="12" fill="#444">to buzzer (see right)</text>')

# ---- table (right)
tx, ty = 760, 110
a(f'<text x="{tx}" y="{ty-20}" font-size="15" font-weight="bold" fill="#111">Connections</text>')
hdr = ["Keypad pin", "Signal", "Heltec"]
a(f'<text x="{tx}" y="{ty}" font-size="12" font-weight="bold" fill="#333">Keypad pin</text>')
a(f'<text x="{tx+85}" y="{ty}" font-size="12" font-weight="bold" fill="#333">Signal</text>')
a(f'<text x="{tx+185}" y="{ty}" font-size="12" font-weight="bold" fill="#333">Heltec</text>')
rows = [("1","R1  1 2 3 A","J3-10  GPIO 39"),("2","R2  4 5 6 B","J3-9   GPIO 40"),
        ("3","R3  7 8 9 C","J3-8   GPIO 41"),("4","R4  * 0 # D","J3-7   GPIO 42"),
        ("5","C1  1 4 7 *","J3-15  GPIO 4"),("6","C2  2 5 8 0","J3-16  GPIO 5"),
        ("7","C3  3 6 9 #","J3-17  GPIO 6"),("8","C4  A B C D","J3-18  GPIO 7")]
for i,(p,s,h) in enumerate(rows):
    y = ty + 22 + i*21
    a(f'<rect x="{tx-8}" y="{y-14}" width="4" height="17" fill="{cols[i]}"/>')
    a(f'<text x="{tx+20}" y="{y}" font-size="12" fill="#111" text-anchor="middle">{p}</text>')
    a(f'<text x="{tx+85}" y="{y}" font-size="12" fill="#111" xml:space="preserve">{s}</text>')
    a(f'<text x="{tx+185}" y="{y}" font-size="12" fill="#111" xml:space="preserve">{h}</text>')

# ---- buzzer inset
ix, iy = 760, 330
a(f'<rect x="{ix-10}" y="{iy-24}" width="320" height="200" rx="8" fill="#fafafa" stroke="#bbb"/>')
a(f'<text x="{ix}" y="{iy-4}" font-size="15" font-weight="bold" fill="#111">Buzzer (ACTIVE type)</text>')
# 3V3 -> buzzer + ; buzzer - -> collector ; base via 1k from J3-13 ; emitter GND
a(f'<text x="{ix+180}" y="{iy+24}" font-size="12" fill="#111">3V3  (J3-2)</text>')
a(f'<line x1="{ix+170}" y1="{iy+20}" x2="{ix+170}" y2="{iy+40}" stroke="#111" stroke-width="2"/>')
a(f'<line x1="{ix+160}" y1="{iy+20}" x2="{ix+178}" y2="{iy+20}" stroke="#111" stroke-width="2"/>')
a(f'<circle cx="{ix+170}" cy="{iy+58}" r="18" fill="#333"/>')
a(f'<text x="{ix+170}" y="{iy+62}" font-size="10" fill="#fff" text-anchor="middle">BZ</text>')
a(f'<text x="{ix+194}" y="{iy+48}" font-size="11" fill="#111">+</text>')
a(f'<text x="{ix+194}" y="{iy+76}" font-size="11" fill="#111">-</text>')
a(f'<line x1="{ix+170}" y1="{iy+76}" x2="{ix+170}" y2="{iy+100}" stroke="#111" stroke-width="2"/>')
# transistor
a(f'<circle cx="{ix+160}" cy="{iy+118}" r="20" fill="none" stroke="#111" stroke-width="2"/>')
a(f'<line x1="{ix+152}" y1="{iy+105}" x2="{ix+152}" y2="{iy+131}" stroke="#111" stroke-width="3"/>')
a(f'<line x1="{ix+152}" y1="{iy+112}" x2="{ix+170}" y2="{iy+100}" stroke="#111" stroke-width="2"/>')
a(f'<line x1="{ix+152}" y1="{iy+124}" x2="{ix+170}" y2="{iy+136}" stroke="#111" stroke-width="2"/>')
a(f'<line x1="{ix+170}" y1="{iy+136}" x2="{ix+170}" y2="{iy+156}" stroke="#111" stroke-width="2"/>')
a(f'<line x1="{ix+160}" y1="{iy+156}" x2="{ix+180}" y2="{iy+156}" stroke="#111" stroke-width="2"/>')
a(f'<text x="{ix+186}" y="{iy+160}" font-size="12" fill="#111">GND  (J3-1)</text>')
a(f'<text x="{ix+186}" y="{iy+122}" font-size="12" fill="#111">2N2222</text>')
a(f'<text x="{ix+186}" y="{iy+104}" font-size="10" fill="#555">C</text>')
a(f'<text x="{ix+186}" y="{iy+142}" font-size="10" fill="#555">E</text>')
# base resistor
a(f'<line x1="{ix+20}" y1="{iy+118}" x2="{ix+60}" y2="{iy+118}" stroke="#111" stroke-width="2"/>')
a(f'<rect x="{ix+60}" y="{iy+111}" width="44" height="14" fill="#fff" stroke="#111" stroke-width="2"/>')
a(f'<text x="{ix+82}" y="{iy+104}" font-size="11" fill="#111" text-anchor="middle">1 kΩ</text>')
a(f'<line x1="{ix+104}" y1="{iy+118}" x2="{ix+152}" y2="{iy+118}" stroke="#111" stroke-width="2"/>')
a(f'<text x="{ix+4}" y="{iy+100}" font-size="12" fill="#111">J3-13</text>')
a(f'<text x="{ix+4}" y="{iy+140}" font-size="12" fill="#111">GPIO 2</text>')
a(f'<text x="{ix}" y="{iy+172}" font-size="11" fill="#555">Use 3V3, not 5V (no 5V on battery).</text>')

# ---- warnings
a(f'<rect x="{jx(4)-7}" y="{jy+54}" width="0" height="0"/>')
ly = 690
a(f'<rect x="30" y="{ly-11}" width="12" height="12" fill="#ffd700" stroke="#000"/><text x="48" y="{ly}" font-size="12" fill="#111">used by V3</text>')
a(f'<rect x="140" y="{ly-11}" width="12" height="12" fill="#ff6b6b" stroke="#000"/><text x="158" y="{ly}" font-size="12" fill="#111">never connect: J3-4 (37 battery sense), J3-5/6 (46/45 strapping), J3-14 (GPIO 3 strapping)</text>')
a(f'<text x="30" y="{ly+20}" font-size="12" fill="#555">Check first: hold key 1 and measure keypad pin 1 to pin 5 in resistance mode (about 100 Ω pressed, open released).</text>')
a('</svg>')
open('/home/user/floodmesh/firmware_v3/docs/keypad-wiring.svg','w').write('\n'.join(o))
