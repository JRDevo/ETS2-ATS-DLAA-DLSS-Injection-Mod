<!-- Short README in Vietnamese. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Chỉ tải mod này từ https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Không có trang web, không có Discord,
> không có Nexus và không có Steam Workshop. Mọi trang, mirror hay "trình cài đặt" khác đưa ra mod này đều không phải của tôi: đừng chạy.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

**DLAA** và **DLSS** của NVIDIA cho **Euro Truck Simulator 2** và **American Truck Simulator**, trên màn hình phẳng và trong VR.
Mod thay anti-aliasing riêng của game bằng DLAA: hàng rào, dây điện, vạch kẻ đường và bóng đổ không còn nhấp nháy.
DLSS cũng có thể upscale hình để có tốc độ khung hình cao hơn.

Chỉ là một file nhỏ, `dinput8.dll`, trong thư mục game. Không launcher, không tài khoản.

- **Hình ổn định, nét:** không nhấp nháy ở hàng rào, dây điện và bóng đổ. Gương cũng được anti-aliasing.
- **Màn hình phẳng và VR:** một file cho cả hai. Ba màn hình và Windows HDR đều chạy được.
- **Tăng tốc VR:** foveated rendering (có cả theo dõi mắt), DLAA chỉ ở giữa mỗi mắt, giới hạn tốc độ khung hình.
- **Menu trong game bằng ngôn ngữ của bạn:** nhấn `Delete`. Mọi cài đặt, phím của nó và mức tốn tài nguyên, kèm mô tả ngắn.
- **Báo cáo lỗi bằng một lần nhấn:** nhấn `Insert` hai lần. Mod lưu hình, thông tin hệ thống và nhật ký vào file zip.

## Bạn cần gì

- Euro Truck Simulator 2 hoặc American Truck Simulator (64-bit, DirectX 11).
- Card đồ họa **NVIDIA RTX** với driver mới. DLAA và DLSS chỉ chạy trên card RTX.
- File `nvngx_dlss.dll` (runtime NVIDIA DLSS). Mod không kèm theo: xem bước 6.
- Với VR: một runtime OpenXR (ví dụ Virtual Desktop) và game được chạy với tùy chọn `-openxr`.

## Cài đặt

1. **Sao lưu** thư mục `bin\win_x64\` của game.
2. [Tải bản phát hành mới nhất](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): file `.zip` dưới **Assets**. Giải nén nó.
3. Mở thư mục game, ví dụ `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (hoặc `...\American Truck Simulator\bin\win_x64\`).
4. **Gỡ các mod proxy khác trước:** xóa `dinput8.dll` hoặc `dxgi.dll` của mod khác trong thư mục đó.
5. Chép **`dinput8.dll`** vào `bin\win_x64\`. Bạn cũng có thể chép **`dlaa.ini`** vào đó để đổi cài đặt và phím (không bắt buộc).
6. Đặt **`nvngx_dlss.dll`** vào cùng thư mục. Lấy từ NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   file `lib/Windows_x86_64/rel/nvngx_dlss.dll`) hoặc chép từ bất kỳ game nào có file này.
7. Khởi động game. DLAA đã bật.

Để gỡ cài đặt: xóa `dinput8.dll` (và `dlaa.ini`) khỏi `bin\win_x64\`.

**Linux / Proton:** cài theo cách tương tự. Trong Steam, đặt tùy chọn khởi chạy của game là `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Các phím quan trọng nhất

| Phím | Chức năng |
|---|---|
| `Delete` | Mở và đóng menu. `Up` / `Down` chọn dòng, `Left` / `Right` đổi giá trị. |
| `End` | Chuyển chế độ: tắt, DLAA, DLSS. |
| `Insert` hai lần | Báo cáo lỗi: lưu hình, thông tin hệ thống và nhật ký vào `dlaa_selftest\`. |

## Thêm

- **Hướng dẫn đầy đủ (tiếng Anh):** [README.md](../../README.md): mọi cài đặt, phím, VR, hiệu năng và xử lý sự cố.
- **Gặp vấn đề:** [mở một issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Vui lòng viết bằng tiếng Anh.
- **Bản dịch** do AI tạo và có thể có lỗi.
  [Báo lỗi bản dịch](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Đây là dự án fan không chính thức. Không liên quan đến SCS Software hay NVIDIA.
