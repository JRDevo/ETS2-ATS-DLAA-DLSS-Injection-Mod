<!-- Short README in Italian. The full guide (English): ../../README.md -->
> [!CAUTION]
> **Scarica questa mod solo da https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases.** Non c'è nessun sito web, nessun Discord,
> nessun Nexus e nessuno Steam Workshop. Qualsiasi altro sito, mirror o «installer» che la offre non è mio: non eseguirlo.

![ETS2 & ATS DLAA & DLSS Mod](../images/banner.jpg)

# ETS2 & ATS: DLAA & DLSS Injection Mod

**DLAA** e **DLSS** di NVIDIA per **Euro Truck Simulator 2** e **American Truck Simulator**, su schermo piatto e in VR.
La mod sostituisce l'anti-aliasing del gioco con il DLAA: recinzioni, fili, segnaletica stradale e ombre non sfarfallano più.
Il DLSS può anche fare l'upscaling dell'immagine per più fotogrammi al secondo.

È un solo piccolo file, `dinput8.dll`, nella cartella del gioco. Nessun launcher, nessun account.

- **Un'immagine stabile e nitida:** niente sfarfallio su recinzioni, fili e ombre. Anche gli specchietti hanno l'anti-aliasing.
- **Schermo piatto e VR:** un solo file per entrambi. Funzionano gli schermi tripli e l'HDR di Windows.
- **Accelerazioni VR:** foveated rendering (anche con eye tracking), DLAA solo al centro di ogni occhio, limiti di frequenza fotogrammi.
- **Un menu nel gioco nella tua lingua:** premi `Delete`. Ogni impostazione, il suo tasto e il suo costo, con una breve descrizione.
- **Una segnalazione bug con un solo gesto:** premi due volte `Insert`. La mod salva immagini, info di sistema e log in file zip.

## Cosa serve

- Euro Truck Simulator 2 o American Truck Simulator (64 bit, DirectX 11).
- Una scheda grafica **NVIDIA RTX** con un driver aggiornato. DLAA e DLSS funzionano solo su schede RTX.
- Il file `nvngx_dlss.dll` (il runtime NVIDIA DLSS). La mod non lo include: vedi il punto 6.
- Per la VR: un runtime OpenXR (per esempio Virtual Desktop) e il gioco avviato con l'opzione `-openxr`.

## Installazione

1. **Fai una copia di backup** della cartella `bin\win_x64\` del gioco.
2. [Scarica l'ultima versione](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest): il `.zip` sotto **Assets**. Decomprimilo.
3. Apri la cartella del gioco, per esempio `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (oppure `...\American Truck Simulator\bin\win_x64\`).
4. **Prima rimuovi le altre mod proxy:** cancella ogni altro `dinput8.dll` o `dxgi.dll` in quella cartella.
5. Copia **`dinput8.dll`** in `bin\win_x64\`. Puoi copiarci anche **`dlaa.ini`** per cambiare impostazioni e tasti (non obbligatorio).
6. Metti **`nvngx_dlss.dll`** nella stessa cartella. Prendilo dall'SDK NVIDIA DLSS (<https://github.com/NVIDIA/DLSS>,
   file `lib/Windows_x86_64/rel/nvngx_dlss.dll`) o copialo da un gioco che lo ha.
7. Avvia il gioco. Il DLAA è attivo.

Per disinstallare: cancella `dinput8.dll` (e `dlaa.ini`) da `bin\win_x64\`.

**Linux / Proton:** installa allo stesso modo. In Steam, imposta le opzioni di avvio del gioco su `WINEDLLOVERRIDES="dinput8=n,b" %command%`.

## I tasti più importanti

| Tasto | Cosa fa |
|---|---|
| `Delete` | Apre e chiude il menu. `Up` / `Down` scelgono una riga, `Left` / `Right` la cambiano. |
| `End` | Cambia il modo: disattivo, DLAA, DLSS. |
| `Insert` due volte | Segnalazione bug: salva immagini, info di sistema e log in `dlaa_selftest\`. |

## Altro

- **La guida completa (inglese):** [README.md](../../README.md): tutte le impostazioni, i tasti, la VR, le prestazioni e la risoluzione dei problemi.
- **Problemi:** [apri una issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose). Scrivila in inglese, per favore.
- **Le traduzioni** sono fatte dall'IA e possono contenere errori.
  [Segnala un problema di traduzione](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new?template=translation.yml).
- Questo è un progetto amatoriale non ufficiale. Non è affiliato a SCS Software né a NVIDIA.
