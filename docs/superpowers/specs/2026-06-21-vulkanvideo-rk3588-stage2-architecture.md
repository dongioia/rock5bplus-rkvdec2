# Spec — Stage 2: RK3588 Vulkan Video Decode → Browser — Architecture Gate

- **Data**: 2026-06-21
- **Status**: PROPOSED — rev.2 (post review indipendente: 4 MAJOR + reframing applicati). Pending user approval prima di `writing-plans`.
- **Scope**: decisione architetturale per come il decode Vulkan-Video funzionante raggiunge il **browser** (Fase B→C) + i task Stage-2 di validazione/abilitazione. NON è l'implementation-plan completo della Fase B (quello ha plan proprio dopo approvazione).
- **Lingua**: IT con termini tecnici EN.
- **Evidenze**: vault `OBSIDIAN_Kernel/VulkanVideo/` — vedi §8.

---

## 0. Contesto + stato attuale

**B0 COMPLETA (19/06)**: l'ICD standalone Vulkan-Video V4L2-backed (`sree/mesa` `5955e6e` + nostre 4 compat-patch + fix init-SPS) decodifica H264 **byte-exact** su RK3588 (kernel 7.1rc1), validato vs ffmpeg (case-1 baseline 120/120, case-2 High+B-frames 60/60, case-3 crop+multi-slice 30/30 visible-normalized). Root cause del blank = control **SPS non-request** mancante all'init V4L2 (`v4l2vk_v4l2_set_init_sps`, commit `e5a0d50`). Display end-to-end OK via `waylandsink` (sway). Upstream: riportato a Mesa #14987 (note 3528237); Nicolas Dufresne ha risposto (AI-disclosure moot; parsing-in-VK = suo problema di spec; il nostro fix init-SPS = sequencing V4L2 **ortogonale**).

**Domanda dello Stage-2 (architecture gate)**: il decode Vulkan funzionante è un **device standalone** (il nostro ICD, separato dalla GPU di rendering). Come arriva al **browser**?

**Evidenza raccolta (sessione 20-21/06, vault-cited)**:
- **Chromium** = singolo `VkPhysicalDevice` (verificato in chromium-147 `VulkanDeviceQueue` = 1 phys/1 dev/1 queue; design-doc single-device; `media/gpu/vulkan` = 0 CL). → un device decode-only separato **non è usabile** da Chromium → servirebbe α (estendere-PanVK) o β (drive-Chromium-multidevice).
- **Firefox** = FFmpeg-PDM only, V4L2 **stateful-M2M only** (rifiuta virtual-M2M Rockchip), GStreamer rimosso FF128, FF153 Vulkan = dGPU-only. → **fuori**. RK3588 solo via MPP-blob.
- **WebKit/GStreamer** = **path senza muro *architetturale*** (a differenza di Chromium) ma con un **blocker di codice da risolvere**. Decode rkvdec OK (il NOSTRO ICD: H264 **byte-exact** in B0 via dump NV12 vs ffmpeg; `v4l2slh264dec` kernel: ~1440fps byte-stream — numero del *kernel decoder*, non del nostro ICD). Handoff decode→GL **negozia e renderizza** standalone (`v4l2slh264dec ! glimagesink`: NV12 dmabuf → GL `texture-target=2D`, **NON** external_oes → quel muro Chromium è Chromium-specifico) — **NB: renderizza + EOS, NON pixel-verificato** attraverso GL/WebKit. Blocker in-browser: negoziazione **VideoMeta** (`v4l2codecs` DMABuf ↔ `webkitglvideosink`), classe WebKit bug-260654 ma **non-fixato per il path v4l2** (*by-absence*: nessuno l'ha mai dimostrato funzionante su questo HW; pty819 su HW identico usa ffmpeg). → dimensione del fix **localizzata ma NON ancora scopata** (può toccare 2 siti: `v4l2codecs` E `webkitglvideosink`).
- **mpv** (non-browser) = HW decode già in produzione → vincita non-browser **bankata**; il browser è la frontiera.

---

## 1. Decisione architetturale

**γ — ICD standalone V4L2-backed Vulkan Video (ADR Option C→B) consumato da browser GStreamer-based (WebKit/Epiphany, WPE), con il fix VideoMeta di `webkitglvideosink` come abilitatore d'integrazione.**

**Rigettati per lo Stage-2** (deferred a ri-valutazione Fase-C):
- **α estendere-PanVK** (same-device): PanVK ha **0** Vulkan-Video decode, nessun piano (Mesa 26.1 sprint = 0 video ext). Lift da zero, coupling Mali≠rkvdec, rischio maintainer alto, agency bassa.
- **β drive-Chromium-multidevice**: richiede **Google CLA** + ribaltare il modello single-device che Chromium ha progettato deliberatamente. Critical path attraverso l'attore meno controllabile.

**Razionale (evidence-backed)**:

| Criterio | α extend-PanVK | β drive-Chromium | **γ GStreamer-browser** | mpv (rif.) |
|---|---|---|---|---|
| Muro architetturale | no (ma driver da zero) | **sì** (single-device by design) | **NO** | n/a |
| Handoff decode→display | interno (da scrivere) | n/a — blocco β è device-model, non handoff | **negozia+renderizza** standalone (texture-target=2D; NON pixel-verif. in GL/WebKit) | funziona |
| Blocker residuo | tutto il driver | Google CLA + redesign | **1 bug VideoMeta fixabile** | nessuno |
| Contribution gate | Mesa DCO + PanVK maintainers | **Google CLA** | WebKit review (Igalia, no CLA) + Mesa DCO | n/a |
| Agency nostra | bassa | ~zero | **alta** (ICD nostro + patch WebKit) | alta |
| Asset riusati | nessuno | ICD | **ICD B0-proven + gst vulkanh264dec + fix localizzato** | — |

γ è il path **più trattabile** — l'unico senza muro *architetturale*, ma **tutti e tre richiedono lavoro reale** (cfr. Analysis "No free browser win on RK3588"). Gira su ciò che **già possediamo**: l'ICD B0-proven (decode byte-exact; **il path display/browser è ancora da provare pixel-corretto**) + l'elemento GStreamer `vulkanh264dec` + un fix WebKit *localizzato ma non ancora dimensionato*. Guidiamo ciò che controlliamo (Mesa ICD + patch WebKit, review-based no-CLA), non Google né il team PanVK.

**NB — decisione = path-da-perseguire, NON chiusa**: γ è scelto come direzione, con **S2.2 (test feed Vulkan, OQ1) come kill-gate**. Se SIA il feed Vulkan SIA il fix VideoMeta si rivelano non-fattibili in tempi utili → si **ri-apre α/β** coi dati raccolti (§5).

**Opzioni considerate, non scelte (γ resta)**: (a) **WPE/`cog`** come target primario invece di Epiphany — più embedded-native (flagship Igalia), stesso backend GStreamer → valutare in OQ3, non cambia γ; (b) **fork WebKit downstream** come stato realistico di Fase B se l'upstream del fix sink è lento — di fatto implicito (§5 row1), comunque più piccolo d'un fork Chromium; (c) **player ffmpeg embedded in shell browser** (come pty819) — bypassa GStreamer ma **abbandona il nostro asset Vulkan** → scartato.

---

## 2. Scope Stage-2 (deliverable)

- **S2.1 — Productionize l'ICD**: consolidare B0 (fix init-SPS + 4 compat-patch) in build riproducibile (`scripts/vvtest/icd-rebuild.sh`, volume `mesa-sree-tree`), deploy isolato (`VK_ICD_FILENAMES`, mesa-pin intatto).
- **S2.2 — Decisione feed decode→display** (primo task di validazione): confrontare in pipeline GStreamer→sink:
  - **V4L2-direct**: `v4l2slh264dec` (kernel rkvdec, no nostro ICD) — già: decode+GL handoff OK standalone; in-browser bloccato da VideoMeta.
  - **Vulkan**: `vulkanh264dec` → **nostro ICD** — negoziazione **diversa** (memoria Vulkan, non v4l2-DMABuf) → può evitare il VideoMeta issue. **NON ancora testato**.
  - Scegliere il feed migliore per il browser.
- **S2.3 — Abilitatore d'integrazione (TIME-BOXED)**: scoprire la **forma minima** del fix VideoMeta + **proof locale** (build di un WebKit/gst patchato sul SBC che fa passare la negoziazione `v4l2codecs`↔`webkitglvideosink`; standalone `glimagesink`/glupload negozia VideoMeta → prova fattibilità). **NON** include un patch pulito/upstreamabile né il landing upstream (→ Fase B). **Attenzione**: può richiedere modifiche **coordinate in DUE siti** (`v4l2codecs` E `webkitglvideosink`) → non assumere "un bug"; + rebuild/deploy WebKit patchato su Beryllium (rischio §5). **Condizionale**: se il feed Vulkan (S2.2/OQ1) evita VideoMeta, S2.3 serve solo per il path V4L2-direct, non per γ-Vulkan.
- **S2.4 — Validazione end-to-end**: un browser GStreamer-based (Epiphany/WebKitGTK, o WPE/cog) su RK3588 riproduce un video H264 con **HW decode** (`/dev/video0` attivo, no SW fallback) e **display corretto** (visual via `grim`).
- **S2.5 — Contribution protocol**: mappare i canali (WebKit = review-based Igalia, no CLA, per il fix sink; Mesa = DCO per l'ICD se upstreamato; nessun Google CLA richiesto su γ).

---

## 3. Criteri di successo — Stage-2 exit gate (GATING)

1a. **(milestone, NON-gating da solo)** In-browser HW decode via `v4l2slh264dec` (kernel rkvdec): un browser GStreamer riproduce H264 con `/dev/video0` attivo + NESSUN SW fallback (`avdec_h264` non plugato) + display non-blank. Prova che il path browser + fix-VideoMeta regge — **ma NON valida il deliverable Vulkan**.
   - **1b. (GATING — il deliverable Stage-2)** In-browser HW decode via il **nostro ICD / feed Vulkan**: marker che lo distingue dal V4L2-direct = `VK_ICD_FILENAMES` settato **E** `vulkanh264dec` (non `v4l2slh264dec`) plugato nel pipeline — `fuser /dev/video0` da solo NON distingue (entrambi bindano rkvdec). Playback con display.
   - **1c. (GATING — correttezza pixel, lezione B0)** Il frame a schermo è **pixel-corretto**, non solo non-blank: catturare un frame (`grim`) di un clip noto e confrontare una regione vs il riferimento ffmpeg **visible-cropped** (attenzione coded-width vs visible-width — pitfall noto `vulkandownload`), OPPURE decodificare un **pattern noto** (SMPTE/checkerboard) e verificarlo. **Blank o garbage = FAIL** (B0: pipeline 48/48→EOS con output blank). `grim` + overlay-che-avanza **NON è sufficiente** (passa anche su video visivamente errato).
2. **Feed documentato**: quale path (V4L2-direct vs Vulkan-ICD) + perché; il fix VideoMeta applicato registrato (forma del patch).
3. **Decisione architetturale registrata** nel vault ADR (`architecture-decision-record.md` aggiornato da candidato a deciso per γ) + contribution path mappato.

**NON-gating (stretch)**: zero-copy Vulkan-present (`vulkansink` cross-device da nostro ICD); upstream del patch WebKit accettato; upstream del fix init-SPS a Mesa.

---

## 4. Open questions / validazioni (= il lavoro)

- **OQ1** [primo test]: il path Vulkan (`vulkanh264dec` → nostro ICD → `vulkansink` zero-copy / `vulkandownload`+`glimagesink` CPU-copy) negozia pulito (evita il VideoMeta issue)? Decide se il feed browser è Vulkan o V4L2-direct.
- **OQ2**: forma minima del fix `webkitglvideosink` VideoMeta + upstreamabilità a Igalia/WebKit. Se Vulkan-feed evita il problema, il fix potrebbe non servire per γ-Vulkan (ma serve per V4L2-direct).
- **OQ3**: quale browser di riferimento — Epiphany/WebKitGTK (full, già installato) vs WPE/`cog` (leggero, embedded-native; `cog` non in repo Beryllium → AUR/build).
- **OQ4**: display path — CPU-copy (`vulkandownload`, B0-proven) come baseline vs zero-copy (`vulkansink`); quest'ultimo = multi-device (nostro ICD → Mali) = stretch.

---

## 5. Rischi + mitigazioni

| Rischio | Prob | Impatto | Mitigazione |
|---|---|---|---|
| Fix `webkitglvideosink` non-banale / non accettato upstream | MED | MED | Carry downstream WebKit patch (più piccolo+upstreamabile di un fork Chromium); standalone glimagesink prova fattibilità |
| Path Vulkan ha un suo issue di negoziazione | MED | MED | Fallback a V4L2-direct + fix VideoMeta (path già provato a livello gst) |
| `cog`/WPE non in repo | BASSO | BASSO | Usare Epiphany (WebKitGTK, già installato 2.52.4) — stesso backend GStreamer |
| Nessuno ha mai fatto WebKit+v4l2sl+rkvdec | — | — | **Frontiera reale** (onestà): è esattamente perché il progetto esiste (guidare il niche RK3588) |
| Browser HW decode resta non-raggiungibile in tempi utili | BASSO | ALTO | mpv resta path HW proven per l'utente nel frattempo (vincita bankata) |
| **Entrambi i feed** (Vulkan E V4L2-direct) falliscono la negoziazione | BASSO-MED | ALTO | **kill-gate S2.2**: dichiarare γ non-percorribile, ri-aprire α/β (deferred) coi dati raccolti — non insistere a oltranza |
| Fix VideoMeta = cambi **a cascata** (`v4l2codecs` + `webkitglvideosink` insieme) | MED | MED | time-box S2.3 (proof locale); se sfora → escalate / fork downstream documentato |
| Output **wrong-but-not-blank** (stride/modifier/crop errato) | MED | MED | gate **1c** pixel-correctness (no pass su solo non-blank) |
| Deploy **WebKit patchato** su Beryllium (rebuild webkitgtk-6.0 2.52.4 / fork) | MED | MED | carry downstream patch documentato (più piccolo d'un fork Chromium); verificare riproducibilità build |

---

## 6. Out of scope (deferred)

- **α/β (Chromium)** — ri-valutare SOLO se `media/gpu/vulkan` materializza con supporto multi-device (0 CL oggi).
- **VP9/HEVC/AV1** — Fase C.
- **Zero-copy Vulkan-present** ottimizzazione — dopo il baseline funzionante.
- **MR Mesa per il fix init-SPS** — follow-up B0 separato (se Sreerenj risponde).
- **Driver impl completo Fase B** — implementation-plan proprio dopo questo gate.

---

## 7. Relazione con l'arco A→B0→B→C

- A ✅ (knowledge-agent) · B0 ✅ (decode byte-exact).
- **Stage-2 (questo) = il gate** che decide l'architettura di B/C.
- **Fase B** (post-gate) = driver H264 reale lungo il path γ scelto (productionize ICD + browser integration).
- **Fase C** = codec completi + zero-copy + eventuale ri-apertura Chromium.

---

## 8. Evidenze (vault) — per il reviewer

- `analyses/Analysis-2026-06-21-webkit-v4l2-decode-rk3588.md` (test WebKit V4L2, root cause VideoMeta).
- `sources/Source-2026-06-21-webkitgtk-decode-capabilities.md` (no-allowlist, bug 260654 — verificati firsthand).
- `sources/Source-2026-06-20-firefox-arm-video-decode-state.md` (Firefox out).
- `sources/Source-2026-06-20-ndufresne-reply-14987.md` (Nicolas: parsing ortogonale).
- `analyses/chromium-vulkan-video-integration-constraints.md`, `analyses/architecture-decision-record.md`, `analyses/strategic-rationale-v4l2-walled-vulkan-bet.md`.
- `wiki/gap-tracker.md` (P0 gates + Architecture Decision Status + browser-target bullet).
