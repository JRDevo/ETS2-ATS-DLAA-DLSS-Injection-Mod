<!-- Short README in Greek. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Κατέβασε αυτό το mod μόνο από το https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Δεν υπάρχει ιστότοπος, Discord,
> Nexus ή Steam Workshop. Κάθε άλλος ιστότοπος, mirror ή «installer» που το προσφέρει δεν είναι δικός μου: μην το τρέξεις.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA **DLAA** και **DLSS** για το **Euro Truck Simulator 2** και το **American Truck Simulator**, σε επίπεδη οθόνη και σε VR.
Το mod αντικαθιστά το δικό του anti-aliasing του παιχνιδιού με DLAA: οι φράχτες, τα καλώδια, οι διαγραμμίσεις και οι σκιές δεν τρεμοπαίζουν πια.
Το DLSS μπορεί επίσης να κάνει upscale της εικόνας για περισσότερα καρέ ανά δευτερόλεπτο.

Είναι ένα μικρό αρχείο, το `dinput8.dll`, στον φάκελο του παιχνιδιού. Χωρίς launcher, χωρίς λογαριασμό.

- **Σταθερή, καθαρή εικόνα:** χωρίς τρεμόπαιγμα σε φράχτες, καλώδια και σκιές. Και οι καθρέφτες παίρνουν anti-aliasing.
- **Επίπεδη οθόνη και VR:** ένα αρχείο για τα δύο. Λειτουργούν τριπλές οθόνες και Windows HDR.
- **Επιταχύνσεις VR:** foveated rendering (και με eye tracking), DLAA μόνο στο κέντρο κάθε ματιού, όρια καρέ.
- **Μενού μέσα στο παιχνίδι στη γλώσσα σου:** πάτα `Delete`. Κάθε ρύθμιση, το πλήκτρο της και το κόστος της, με σύντομη περιγραφή.
- **Αναφορά σφάλματος με ένα πάτημα:** πάτα δύο φορές `Insert`. Το mod αποθηκεύει εικόνες, πληροφορίες συστήματος και logs σε zip.

## Τι χρειάζεσαι

- Euro Truck Simulator 2 ή American Truck Simulator (64-bit, DirectX 11).
- Μια κάρτα γραφικών **NVIDIA RTX** με πρόσφατο driver. Το DLAA και το DLSS τρέχουν μόνο σε κάρτες RTX.
- Το αρχείο `nvngx_dlss.dll` (το runtime NVIDIA DLSS). Το mod δεν το περιλαμβάνει: δες το βήμα 6.
- Για VR: ένα runtime OpenXR (για παράδειγμα Virtual Desktop) και το παιχνίδι ξεκινημένο με την επιλογή `-openxr`.

## Εγκατάσταση

1. **Κάνε αντίγραφο ασφαλείας** του φακέλου `bin\win_x64\` του παιχνιδιού.
2. [Κατέβασε την τελευταία έκδοση](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): το `.zip` κάτω από το **Assets**. Αποσυμπίεσέ το.
3. Άνοιξε τον φάκελο του παιχνιδιού, για παράδειγμα `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (ή `...\American Truck Simulator\bin\win_x64\`).
4. **Αφαίρεσε πρώτα άλλα proxy mods:** σβήσε κάθε άλλο `dinput8.dll` ή `dxgi.dll` σε αυτόν τον φάκελο.
5. Αντίγραψε το **`dinput8.dll`** στο `bin\win_x64\`. Μπορείς να αντιγράψεις εκεί και το **`dlaa.ini`** για να αλλάξεις ρυθμίσεις και πλήκτρα (δεν είναι υποχρεωτικό).
6. Βάλε το **`nvngx_dlss.dll`** στον ίδιο φάκελο. Πάρε το από το NVIDIA DLSS SDK (<https://github.com/NVIDIA/DLSS>,
   αρχείο `lib/Windows_x86_64/rel/nvngx_dlss.dll`) ή αντίγραψέ το από οποιοδήποτε παιχνίδι το έχει.
7. Ξεκίνα το παιχνίδι. Το DLAA είναι ενεργό.

Για απεγκατάσταση: σβήσε το `dinput8.dll` (και το `dlaa.ini`) από το `bin\win_x64\`.

**Linux / Proton:** εγκατάστησέ το με τον ίδιο τρόπο. Στο Steam, βάλε στις επιλογές εκκίνησης του παιχνιδιού `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## Τα πιο σημαντικά πλήκτρα

| Πλήκτρο | Τι κάνει |
|---|---|
| `Delete` | Ανοίγει και κλείνει το μενού. `Up` / `Down` επιλέγουν γραμμή, `Left` / `Right` την αλλάζουν. |
| `End` | Αλλάζει τη λειτουργία: ανενεργό, DLAA, DLSS. |
| `Insert` δύο φορές | Αναφορά σφάλματος: αποθηκεύει εικόνες, πληροφορίες συστήματος και logs στο `dlaa_selftest\`. |

## Περισσότερα

- **Ο πλήρης οδηγός (στα αγγλικά):** [README.md](../../README.md): όλες οι ρυθμίσεις, τα πλήκτρα, το VR, η απόδοση και η αντιμετώπιση προβλημάτων.
- **Προβλήματα:** [άνοιξε ένα issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Παρακαλώ γράψε το στα αγγλικά.
- **Οι μεταφράσεις** γίνονται από AI και μπορεί να έχουν λάθη.
  [Αναφορά προβλήματος μετάφρασης](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Αυτό είναι ένα ανεπίσημο fan project. Δεν σχετίζεται με την SCS Software ή την NVIDIA.
