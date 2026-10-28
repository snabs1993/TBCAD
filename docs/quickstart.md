# TBCAD Quick Start / คู่มือเริ่มต้นใช้งาน

TBCAD is a free 2D CAD program (MIT licence). This page is also in the program: **Help > Quick Start**.
TBCAD คือโปรแกรม CAD 2 มิติฟรี เปิดได้จากเมนู **Help > Quick Start**

## English

### 1. Draw
- Type a command at the bottom (or use the menus and the tool strip). **Enter** or **Space** accepts, **Esc** cancels, **Enter** on an empty line repeats the last command.
- Basic shapes: `LINE` (L), `CIRCLE` (C), `ARC` (A), `PLINE`, `RECTANG`, `ELLIPSE`, `HATCH`.
- Type points as `10,20` (absolute), `@5,0` (relative) or `@10<45` (distance and angle).
- Mouse wheel zooms, the middle button pans, double middle-click zooms to everything.

### 2. Edit
- `MOVE` (M), `COPY` (CO), `ROTATE` (RO), `SCALE` (SC), `MIRROR` (MI), `ERASE` (E), `ARRAY`.
- `OFFSET` (O), `TRIM` (TR), `EXTEND` (EX), `FILLET` (F), `CHAMFER`.
- Click objects, or drag a rectangle; hold Alt while dragging for a free-form lasso. `Ctrl+Z` undoes, `Ctrl+Y` redoes.

### 3. Drawing aids (function keys)
F3 object snap, F7 grid, F8 ortho, F9 grid snap, F10 polar tracking. Use `OSNAP` to choose snap modes.

### 4. Layers, text and dimensions
- `LAYER` opens the layer panel (colour, linetype, lock, freeze; sub-layers are supported).
- `TEXT` and `MTEXT` write text; Thai text works. `TEXTSTYLES` manages text styles.
- `DIMLINEAR`, `DIMALIGNED`, `DIMANGULAR`, `DIMRADIUS`, `DIMDIAMETER` add dimensions; `DIMSTYLES` manages their styles.

### 5. Blocks and reuse
- `BLOCK` makes a block, `INSERT` places one.
- **Content Browser** (`CONTENT`): open another `.tbcad`, `.tbt` or `.dxf` file and drag its blocks, layers and styles into your drawing.
- **Tool Palettes** (`PALETTES`): keep your favourite blocks and hatches as one-click tools; drag blocks from the Content Browser onto a palette.

### 6. Files
- `Ctrl+S` saves a `.tbcad` file. `OPEN` opens one. `IMPORTDXF` / `EXPORTDXF` exchange DXF files.
- `PLOT` prints; `EXPORTPDF` writes a PDF.
- Templates: `SAVEASTEMPLATE`, `NEWFROMTEMPLATE`. New drawings start from the default template (standard layers and styles).

### 7. Make it yours
- **Settings** (Edit menu): command aliases (import an acad.pgp-style file with `ALIASIMPORT`) and keyboard shortcuts.
- **Workspaces** (status bar, or `WORKSPACE`): save and switch panel layouts.

### 8. Common shortcuts
`Ctrl+N` new, `Ctrl+O` open, `Ctrl+S` save, `Ctrl+Z` / `Ctrl+Y` undo / redo, `Ctrl+C` / `Ctrl+V` copy / paste, `Ctrl+A` select all, `Ctrl+L` command log, `F1` help for the running command (or this page), `F2` text window, `F11` clean screen. **Help > Command List** lists every command.

### 9. Tell us what breaks
Use **Help > Report a Problem...**: note what you did, what you expected, and what happened. **Help > About** shows the version and build number to include.

### 10. Installing and where your files live
- The installer is not code-signed yet, so Windows shows a blue **"Windows protected your PC"** (SmartScreen) window the first time. This is expected and does not mean the file is bad. Click **More info** (small link in the text), then click the **Run anyway** button that appears. If the browser first says the file "is not commonly downloaded", choose **Keep** in its menu. This only happens for the installer, not for the program afterwards.
- Settings (aliases, shortcuts, workspaces) are in `%APPDATA%\TBCAD`. Autosave copies and crash reports are in `%LOCALAPPDATA%\TBCAD` (folders `autosave` and `crashes`). Paste these into the Windows Explorer address bar to open them.
- TBCAD autosaves every 10 minutes. After a crash it offers to recover the last autosave when you start it again.
- If something goes wrong, use **Help > Report a Problem...** and attach the newest file from the `crashes` folder if there is one.

### 11. Finding your way around
- **Toolbox** (under the menu bar): tabs **Draw, Modify, Annotate, Blocks, Inspect, Output**. Click a tool to run it; a small arrow on a tool opens its other versions (for example other ways to draw a circle). Hover for a tip.
- **Search** (`Ctrl+K`, or click the box in the middle of the top bar): type part of a command name, or a Thai word such as `วงกลม`, `ย้าย`, `ตัด`, then pick the command from the list.
- **Quick rail** (slim bar down the left edge): the six tools you use all the time (Line, Polyline, Circle, Move, Trim, Erase), available from every Toolbox tab. Right-click any Toolbox tool and choose **Add to Quick rail** or **Remove from Quick rail**. Hide the rail from the **View** menu.
- **Second monitor**: drag a drawing tab out of the program window and let go on another screen. The drawing opens in its own TBCAD window there. Drop the tab onto another TBCAD window to move the drawing into it.

## ภาษาไทย

### 1. วาด
- พิมพ์คำสั่งที่ช่องด้านล่าง (หรือใช้เมนูและแถบเครื่องมือ) กด **Enter** หรือ **Space** เพื่อยืนยัน **Esc** เพื่อยกเลิก และกด **Enter** บรรทัดว่างเพื่อทำคำสั่งล่าสุดซ้ำ
- รูปทรงพื้นฐาน: `LINE` (L), `CIRCLE` (C), `ARC` (A), `PLINE`, `RECTANG`, `ELLIPSE`, `HATCH`
- ใส่พิกัดเป็น `10,20` (สัมบูรณ์), `@5,0` (สัมพัทธ์) หรือ `@10<45` (ระยะและมุม)
- ลูกกลิ้งเมาส์ซูม ปุ่มกลางเลื่อนภาพ ดับเบิลคลิกปุ่มกลางเพื่อซูมให้เห็นทั้งหมด

### 2. แก้ไข
- `MOVE` (M), `COPY` (CO), `ROTATE` (RO), `SCALE` (SC), `MIRROR` (MI), `ERASE` (E), `ARRAY`
- `OFFSET` (O), `TRIM` (TR), `EXTEND` (EX), `FILLET` (F), `CHAMFER`
- คลิกเลือกวัตถุ หรือลากกรอบสี่เหลี่ยม กด Alt ค้างตอนลากเพื่อเลือกแบบวาดเส้นอิสระ `Ctrl+Z` ย้อนกลับ `Ctrl+Y` ทำซ้ำ

### 3. ตัวช่วยวาด (ปุ่ม F)
F3 จับจุด F7 ตาราง F8 วาดตั้งฉาก (Ortho) F9 จับตาราง F10 ติดตามมุม ใช้ `OSNAP` เลือกชนิดจุดที่จับ

### 4. เลเยอร์ ข้อความ และขนาด
- `LAYER` เปิดแผงเลเยอร์ (สี ชนิดเส้น ล็อก ปิดการมองเห็น และมีเลเยอร์ย่อย)
- `TEXT` และ `MTEXT` เขียนข้อความ ใช้ภาษาไทยได้ `TEXTSTYLES` จัดการแบบอักษร
- `DIMLINEAR`, `DIMALIGNED`, `DIMANGULAR`, `DIMRADIUS`, `DIMDIAMETER` ใส่ขนาด `DIMSTYLES` จัดการลักษณะของขนาด

### 5. บล็อกและการใช้ซ้ำ
- `BLOCK` สร้างบล็อก `INSERT` วางบล็อก
- **Content Browser** (`CONTENT`): เปิดไฟล์ `.tbcad` `.tbt` หรือ `.dxf` อื่น แล้วลากบล็อก เลเยอร์ และสไตล์เข้ามาในแบบของคุณ
- **Tool Palettes** (`PALETTES`): เก็บบล็อกและลายแรเงาที่ใช้บ่อยเป็นปุ่มคลิกเดียว ลากบล็อกจาก Content Browser ไปวางบนพาเลตต์ได้

### 6. ไฟล์
- `Ctrl+S` บันทึกเป็นไฟล์ `.tbcad` `OPEN` เปิดไฟล์ `IMPORTDXF` / `EXPORTDXF` แลกเปลี่ยนไฟล์ DXF
- `PLOT` พิมพ์ `EXPORTPDF` สร้างไฟล์ PDF
- เทมเพลต: `SAVEASTEMPLATE`, `NEWFROMTEMPLATE` แบบใหม่เริ่มจากเทมเพลตเริ่มต้น (มีเลเยอร์และสไตล์มาตรฐาน)

### 7. ปรับให้เป็นของคุณ
- **Settings** (เมนู Edit): ชื่อย่อคำสั่ง (นำเข้าไฟล์แบบ acad.pgp ด้วย `ALIASIMPORT`) และปุ่มลัด
- **Workspaces** (แถบสถานะ หรือ `WORKSPACE`): บันทึกและสลับการจัดวางแผง

### 8. ปุ่มลัดที่ใช้บ่อย
`Ctrl+N` ใหม่, `Ctrl+O` เปิด, `Ctrl+S` บันทึก, `Ctrl+Z` / `Ctrl+Y` ย้อนกลับ / ทำซ้ำ, `Ctrl+C` / `Ctrl+V` คัดลอก / วาง, `Ctrl+A` เลือกทั้งหมด, `Ctrl+L` บันทึกคำสั่ง, `F1` วิธีใช้คำสั่งที่กำลังทำ (หรือหน้านี้), `F2` หน้าต่างข้อความ, `F11` ซ่อนแผงทั้งหมด **Help > Command List** แสดงคำสั่งทั้งหมด

### 9. ถ้าเจอปัญหา
ใช้ **Help > Report a Problem...** จดว่าทำอะไร คาดหวังอะไร และเกิดอะไรขึ้น แล้วดูเวอร์ชันและเลขบิลด์ได้ที่ **Help > About**

### 10. การติดตั้งและที่เก็บไฟล์
- ตัวติดตั้งยังไม่ได้ลงลายเซ็นดิจิทัล ครั้งแรก Windows จะขึ้นหน้าต่างสีน้ำเงิน **"Windows protected your PC"** (SmartScreen) เป็นเรื่องปกติ ไม่ได้แปลว่าไฟล์เสีย ให้กด **More info** (ลิงก์ตัวเล็กในข้อความ) แล้วกดปุ่ม **Run anyway** ที่โผล่ขึ้นมา ถ้าเบราว์เซอร์ขึ้นว่าไฟล์ "ไม่ค่อยมีคนดาวน์โหลด" ให้เลือก **Keep** (เก็บไว้) ในเมนูของมัน จะขึ้นเฉพาะตอนติดตั้ง ไม่ขึ้นตอนเปิดโปรแกรม
- การตั้งค่า (ชื่อย่อคำสั่ง ปุ่มลัด เวิร์กสเปซ) อยู่ที่ `%APPDATA%\TBCAD` ส่วนไฟล์บันทึกอัตโนมัติและรายงานการล่มอยู่ที่ `%LOCALAPPDATA%\TBCAD` (โฟลเดอร์ `autosave` และ `crashes`) วางที่อยู่นี้ในช่อง address ของ Explorer เพื่อเปิด
- TBCAD บันทึกอัตโนมัติทุก 10 นาที ถ้าโปรแกรมล่ม เปิดใหม่จะถามว่าจะกู้ไฟล์ล่าสุดหรือไม่
- ถ้าเจอปัญหา ใช้ **Help > Report a Problem...** และแนบไฟล์ล่าสุดในโฟลเดอร์ `crashes` (ถ้ามี)

### 11. หาคำสั่งและจัดหน้าจอ
- **Toolbox** (ใต้แถบเมนู): มีแท็บ **Draw, Modify, Annotate, Blocks, Inspect, Output** คลิกเครื่องมือเพื่อใช้งาน ถ้ามีลูกศรเล็กที่เครื่องมือ กดแล้วจะมีแบบอื่นให้เลือก (เช่น วิธีวาดวงกลมแบบอื่น) เอาเมาส์ชี้เพื่อดูคำอธิบาย
- **ค้นหา** (`Ctrl+K` หรือคลิกช่องกลางแถบบน): พิมพ์ส่วนหนึ่งของชื่อคำสั่ง หรือคำไทย เช่น `วงกลม` `ย้าย` `ตัด` แล้วเลือกคำสั่งจากรายการ
- **Quick rail** (แถบเล็กทางซ้ายสุด): เครื่องมือ 6 ตัวที่ใช้บ่อย (Line, Polyline, Circle, Move, Trim, Erase) กดได้จากทุกแท็บของ Toolbox คลิกขวาที่เครื่องมือใน Toolbox แล้วเลือก **Add to Quick rail** หรือ **Remove from Quick rail** เพื่อเพิ่ม/เอาออก ซ่อนแถบได้จากเมนู **View**
- **จอที่สอง**: ลากแท็บแบบวาดออกนอกหน้าต่างโปรแกรม แล้วปล่อยบนจออีกใบ แบบนั้นจะเปิดเป็นหน้าต่าง TBCAD ใหม่บนจอนั้น ปล่อยบนหน้าต่าง TBCAD อีกอัน แบบจะย้ายเข้าไปในหน้าต่างนั้น
