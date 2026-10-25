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
