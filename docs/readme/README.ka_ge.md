<!-- Short README in Georgian. The full guide (English): ../../README.md -->
> [!CAUTION]
> **ეს მოდი ჩამოტვირთე მხოლოდ აქედან: https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** არ არსებობს ვებსაიტი, Discord,
> Nexus და Steam Workshop. ნებისმიერი სხვა საიტი, სარკე ან „ინსტალერი“, რომელიც მას გთავაზობს, ჩემი არ არის: არ გაუშვა.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA-ს **DLAA** და **DLSS** თამაშებისთვის **Euro Truck Simulator 2** და **American Truck Simulator**, ბრტყელ ეკრანზე და VR-ში.
მოდი თამაშის საკუთარ ანტიალიასინგს DLAA-ით ცვლის: ღობეები, სადენები, გზის ხაზები და ჩრდილები აღარ ციმციმებს.
DLSS-ს შეუძლია სურათის გაზრდაც უფრო მაღალი კადრების სიხშირისთვის.

ეს ერთი პატარა ფაილია, `dinput8.dll`, თამაშის საქაღალდეში. ლაუნჩერისა და ექაუნთის გარეშე.

- **სტაბილური, მკვეთრი სურათი:** ღობეებზე, სადენებსა და ჩრდილებზე ციმციმი არ არის. სარკეებიც იღებენ ანტიალიასინგს.
- **ბრტყელი და VR:** ერთი ფაილი ორივესთვის. სამი ეკრანი და Windows HDR მუშაობს.
- **VR აჩქარებები:** foveated rendering (თვალთვალითაც), DLAA მხოლოდ თითოეული თვალის ცენტრში, კადრების სიხშირის ლიმიტები.
- **თამაშში მენიუ შენს ენაზე:** დააჭირე `Delete`-ს. ყველა პარამეტრი, მისი კლავიში და ღირებულება, მოკლე აღწერით.
- **შეცდომის ანგარიში ერთი დაჭერით:** დააჭირე `Insert`-ს ორჯერ. მოდი სურათებს, სისტემის ინფოსა და ჟურნალებს zip-ებში ინახავს.

## რა გჭირდება

- Euro Truck Simulator 2 ან American Truck Simulator (64-ბიტიანი, DirectX 11).
- **NVIDIA RTX** ვიდეოქარდი მიმდინარე დრაივერით. DLAA და DLSS მუშაობს მხოლოდ RTX ქარდებზე.
- ფაილი `nvngx_dlss.dll` (NVIDIA DLSS runtime). მოდი მას არ შეიცავს: იხილე ნაბიჯი 6.
- VR-ისთვის: OpenXR runtime (მაგალითად Virtual Desktop) და თამაში გაშვებული `-openxr` პარამეტრით.

## ინსტალაცია

1. **გააკეთე სარეზერვო ასლი** თამაშის `bin\win_x64\` საქაღალდისა.
2. [ჩამოტვირთე უახლესი ვერსია](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): `.zip` **Assets**-ის ქვეშ. ამოარქივე.
3. გახსენი თამაშის საქაღალდე, მაგალითად `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (ან `...\American Truck Simulator\bin\win_x64\`).
4. **ჯერ წაშალე სხვა proxy მოდები:** წაშალე ამ საქაღალდეში სხვა მოდის ნებისმიერი `dinput8.dll` ან `dxgi.dll`.
5. დააკოპირე **`dinput8.dll`** საქაღალდეში `bin\win_x64\`. შეგიძლია იქვე დააკოპირო **`dlaa.ini`** პარამეტრებისა და კლავიშების შესაცვლელად (სავალდებულო არ არის).
6. ჩადე **`nvngx_dlss.dll`** იმავე საქაღალდეში. აიღე NVIDIA DLSS SDK-დან (<https://github.com/NVIDIA/DLSS>,
   ფაილი `lib/Windows_x86_64/rel/nvngx_dlss.dll`) ან დააკოპირე ნებისმიერი თამაშიდან, რომელსაც ის აქვს.
7. გაუშვი თამაში. DLAA ჩართულია.

წასაშლელად: წაშალე `dinput8.dll` (და `dlaa.ini`) საქაღალდიდან `bin\win_x64\`.

**Linux / Proton:** დააყენე ანალოგიურად. Steam-ში თამაშის გაშვების პარამეტრებში ჩაწერე `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## ყველაზე მნიშვნელოვანი კლავიშები

| კლავიში | რას აკეთებს |
|---|---|
| `Delete` | ხსნის და ხურავს მენიუს. `Up` / `Down` ირჩევს სტრიქონს, `Left` / `Right` ცვლის მას. |
| `End` | ცვლის რეჟიმს: გამორთ., DLAA, DLSS. |
| `Insert` ორჯერ | შეცდომის ანგარიში: ინახავს სურათებს, სისტემის ინფოსა და ჟურნალებს `dlaa_selftest\`-ში. |

## მეტი

- **სრული გზამკვლევი (ინგლისურად):** [README.md](../../README.md): ყველა პარამეტრი, კლავიშები, VR, წარმადობა და პრობლემების გადაჭრა.
- **პრობლემები:** [გახსენი issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). გთხოვ, დაწერე ინგლისურად.
- **თარგმანებს** ხელოვნური ინტელექტი აკეთებს და შეიძლება შეცდომებს შეიცავდეს.
  [შეატყობინე თარგმანის პრობლემა](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- ეს არაოფიციალური ფანების პროექტია. არ არის დაკავშირებული SCS Software-თან ან NVIDIA-სთან.
