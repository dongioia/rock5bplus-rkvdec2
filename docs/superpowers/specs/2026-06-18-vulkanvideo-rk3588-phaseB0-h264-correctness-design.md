# Spec — Fase B0: RK3588 Vulkan Video H264 Decode — Correctness Spike

- **Data**: 2026-06-18 (rev. 4 — post review utente P0/P1/P2; rev.3 = brainstorm + protocol review + review utente + final-gate)
- **Status**: APPROVATA per implementazione (P0/P1/P2-fix documentali applicati). Stage 1 (correttezza) = focus. Stage 2 (architecture gate) GATED / parallelo-solo-documentale.
- **Scope**: SOLO B0, singolo implementation-plan. Stage 2 ha spec propria quando innescato.
- **Lingua**: IT con termini tecnici EN.

---

## 0. Contesto + stato attuale

Fase A completa: knowledge-agent + vault `OBSIDIAN_Kernel/VulkanVideo/` (continuo cloud attivo). Il prototipo Mesa Vulkan-Video V4L2 ICD (`sree/mesa` commit `5955e6eb`, branch `v4l2-vulkan-video`) è un **experimental PoC (parsing stopgap)**: apre-codifica un parser bitstream in-ICD per parametri non veicolati dall'API Vulkan Video (per VP9 le probabilities; per H264 `dec_ref_pic_marking_bit_size`, `pic_order_cnt_bit_size`). Costruito in `rock5b-dev` (3 source-patch + ICD JSON scritto a mano, **review CLEAN**) e deployato **isolato** su Rock5B+ (kernel 7.1rc1, BredOS).

**Stato verificato sul NOSTRO build**: ICD carica (ABI ok), `vulkaninfo` enumera "V4L2 Vulkan Video Decoder", binda rkvdec `/dev/video0`, espone `VK_KHR_video_decode_h264/queue`; `gst vulkanh264dec` registra; pipeline `filesrc ! h264parse ! vulkanh264dec ! vulkandownload ! NV12 ! fakesink` gira **48/48 frame → EOS, no crash**. **MA output BLANK** (entropy frame0 luma = 2 valori unici vs ~226 del riferimento ffmpeg; tutti i frame near-blank).

**Frame strategico** (vincola priorità, non correttezza): V4L2-direct provato fuori-browser ma **Chromium-walled**; Vulkan Video = la scommessa browser MA RK3588 nicchia → upstream non prioritizza → **dobbiamo guidarlo noi**. Parsing = stopgap (Nicolas); extension non-parsing = target futuro ipotetico. Design-doc Khronos↔Chromium: single-device GPU-native, V4L2 assente, standalone-ICD non indirizzato.

**Glossario gate** (definiti nella Phase-A roadmap, qui per autonomia): **Gate 5** = qualità del mapping/decode H264; **Gate 2** = compatibilità formato/modifier (rkvdec↔sampling). Gate 5 = HIGH sul **riferimento di sree** ma **MEDIUM / NON-CONFERMATO sul NOSTRO build** (blank). **B0 deve riprodurre la correttezza** — non eredita l'HIGH (anti-allucinazione).

**COPY2** = (in questo doc) lo step di copia `mmap CAPTURE → VkImage` dentro l'ICD (`v4l2vk_vk_device.c`, in `QueueSubmit2`). COPY3 = `VkImage → VkBuffer` (CmdCopyImageToBuffer2).

---

## 1. Obiettivo + criteri di successo

**Obiettivo**: rendere il decode H264 **pixel-corretto** via l'ICD Vulkan-Video V4L2-backed standalone sul nostro RK3588 (kernel 7.1rc1), validato con GStreamer + riferimento; documentare la root-cause; produrre gli input per Stage 2.

**Criteri di successo — Stage-1 exit (GATING, tutti, verificabili in isolamento)**:
1. **v4l2-tracer control-match**: i VALORI dei controlli V4L2 del nostro ICD su 1 I-frame (`SPS` incl. `level_idc` raw, `PPS`, `DECODE_PARAMS.dpb[].reference_ts`, timestamp `QBUF`, presenza `SCALING_MATRIX`) **coincidono** col golden trace di `v4l2slh264dec` (FRESH, stesso SBC/kernel). Confronto sui VALORI, non sulla struttura grezza (R5).
2. **DPB binding & sequencing corretto**: (a) per ogni P/B-frame il `dpb[].reference_ts` risolve al CAPTURE buffer effettivamente DQBUF-ato con quel timestamp; (b) il numero di reference attive ≤ `num_ref_frames` (SPS); (c) le unità timestamp sono coerenti OUTPUT↔CAPTURE (no ms/ns/frame-index mismatch); (d) i controlli sono associati al request V4L2 corretto e l'ordinamento request è valido (P1.8, §6 Step 2). *(How-to: §6 Step 2.)*
3. **Output corretto (byte-exact, visible-normalized)**: l'output del nostro ICD via GStreamer, **stride-normalized e croppato alla VISIBLE size** `Wv×Hv` (procedura §6.B), confrontato col riferimento ffmpeg (che applica già il crop):
   - **PRIMARIO**: **byte-exact** sui campioni NV12 visible-normalized. Il decode H264 è normativo (samples bit-exact) → atteso byte-identico vs ffmpeg conformante sul medesimo bitstream.
   - **Fallback**: `PSNR ≥ 50 dB` **solo se il residuo è spiegato** (documentare la causa, es. siting chroma noto). `40 ≤ PSNR < 50` = **diagnostico, NON pass**. Output blank = **fail immediato**.
4. **Root cause documentata** (quale ipotesi/confine) + patch del fix registrate.

**Stretch (best-effort, NON-gating)**: subset VK-CTS `dEQP-VK.video.decode.h264.*`. Se lo stub ICD non regge il setup CTS (probabile — richiede ICD molto più completo), documentare come **gate di Fase B**, non bloccare B0.

---

## 2. Decomposizione (2 stage)

- **Stage 1 — Correttezza** (il gate di questo spike). Focus.
- **Stage 2 — Architecture decision gate** (GATED). Standalone-ICD vs estendere-PanVK (Opz. A) vs DMA-BUF-interop. Spec propria quando innescato.

**Invariante**: la correttezza V4L2→output di Stage-1 è **prerequisito di TUTTE** le architetture (architecture-independent) → non sprecata anche se Stage-2 conclude che lo standalone è browser-dead.

---

## 3. Architettura / ambiente (workflow provato)

- **Build**: Docker `rock5b-dev` (Arch Linux ARM, aarch64 nativo) su volume `mesa-sree-tree` (`sree/mesa` 5955e6eb + `compat-mesa26.patch`). Throwaway/locale. Sorgenti solo in volume.
- **Deploy**: scp a SBC `~/vvtest/`, **isolato** via `VK_ICD_FILENAMES` (no system `icd.d`, no sudo, **mesa-pin 26.0.6 intatto**). Provato safe.
- **Contribution gate: CHIUSO** (locale/throwaway, no outbound). Kernel-bug o fix upstreamabile → decisione gated separata (humanizer + AI-disclosure + reviewer).

---

## 4. Precondizioni (BLOCCANTI, a SBC accesa)

Nessuno step di debug parte finché entrambe le precondizioni non sono soddisfatte e salvate in `artifacts/phase-b0/metadata/` (§8).

### Precondizione 0 — SBC availability gate (P0.1)
La SBC era **down il 2026-06-18**. B0 richiede SBC accesa e raggiungibile. Checklist (fail su una qualsiasi → STOP, non procedere):
```
ssh rock5b 'true'                                  # raggiungibile
ssh rock5b 'uname -a'                              # kernel/arch
ssh rock5b 'gst-launch-1.0 --version'              # GStreamer (R10)
ssh rock5b 'ls -l /dev/video0 /dev/media*'         # device V4L2 presenti
ssh rock5b 'which v4l2-tracer v4l2-ctl ffmpeg'     # tool gating
ssh rock5b 'gst-inspect-1.0 v4l2slh264dec >/dev/null && echo v4l2slh264dec OK'   # golden decoder funziona
ssh rock5b 'which h264_analyze 2>/dev/null || echo "h264bitstream (AUR) — solo se serve §6 Step 3"'
# sample H264 presente + hash registrato:
ssh rock5b 'sha256sum ~/vvtest/test.h264'
```
Tool gating mancanti → installare (`v4l-utils` per `v4l2-tracer`/`v4l2-ctl`; `h264bitstream` da AUR opzionale) prima di proseguire (R4).

### Precondizione 1 — Driver identity (P2, registrare PRIMA degli step)
"rkvdec" varia per kernel/branch/BSP → registrare l'identità reale del device e i formati esposti, salvare in `artifacts/phase-b0/metadata/`:
```
ssh rock5b 'uname -a'
ssh rock5b 'v4l2-ctl -d /dev/video0 --info'                 # driver / bus_info / card
ssh rock5b 'media-ctl -p'                                   # topologia + entità
ssh rock5b 'cat /sys/class/video4linux/video0/name'
ssh rock5b 'tr -d "\0" < /sys/class/video4linux/video0/device/of_node/compatible 2>/dev/null || echo "(of_node compatible non esposto)"'   # DT compatible (es. rockchip,rk3588-vdpu381)
ssh rock5b 'v4l2-ctl -d /dev/video0 --list-formats-ext'     # formati OUTPUT/CAPTURE supportati
```

---

## 5. Stream di riferimento + corpus (precondizione dati)

File H264 esatto (path) + **SHA256** + provenienza nel findings. **Annex-B `.h264` con primo AU = IDR + SPS/PPS in-band** (in pipeline si aggiunge `h264parse config-interval=1` per garantire SPS/PPS in ogni AU — §6 Step 1). Stesso file per `ours` e golden. Golden (`v4l2slh264dec` trace + output) **rigenerato FRESH sullo stesso SBC/kernel** (R5, R10). Documentare versione GStreamer.

**Corpus B0 (limitato, in ordine di chiusura)**:
1. **Caso minimo** — IDR-only o IDR+P, progressive, 8-bit 4:2:0, no-interlace, no-MBAFF. B0 **chiude su questo**.
2. **Reordering** — P/B con reference reordering (esercita DPB/POC + binding §1.2).
3. **Multi-slice / crop non banale** — esercita §6.B visible-crop + per-request multi-slice (P1.8).

Casi 2/3 = best-effort se il tempo regge; non bloccano l'exit di B0 una volta che il caso 1 è byte-exact.

---

## 6. Metodo di debug Stage-1 (systematic-debugging)

### Ipotesi
- **H-FORMAT** — formato/modifier CAPTURE rkvdec ≠ lineare assunto dal readback (stride/tiling/multi-plane) → memcpy legge garbage.
- **H-CONTROL** — valori/layout controlli V4L2 errati (incl. ABI-drift struct `v4l2_ctrl_h264_*` kernel-7.1rc1 vs header branch sree — R6) **e/o** controlli non associati al request corretto (P1.8).
- **H-PARSER** — parser bitstream in-ICD produce valori errati. Campi first-class da verificare: `pic_order_cnt_bit_size`, `dec_ref_pic_marking_bit_size`, presenza MMCO, `frame_num`, `pic_order_cnt_lsb`, `idr_pic_id`, flag IDR/reference/field; exp-Golomb; SEI / `gaps_in_frame_num_value_allowed_flag`.
- **H-DPB** — binding & sequencing DPB: `reference_ts` numericamente giusto ma risolve al buffer sbagliato; ordine QBUF/DQBUF; unità timestamp; buffer count; **per-request V4L2 stateless** (P1.8).
- **H-GST-NEGO** — `vulkanh264dec`/`vulkandownload` negoziano male con l'ICD (query `VkVideoProfileInfoKHR`/`VkVideoDecodeCapabilitiesKHR`/`VkImageFormatProperties`/memory-type/DPB DISTINCT-vs-COINCIDE) → path blank a monte del copy.

### Step 0 — Fast boundary probe (decide l'entry economica; P1 debug-order)
Il bug vive su uno di due assi:
- **asse dati**: `CAPTURE buffer → COPY2 → VkImage → vulkandownload` (output).
- **asse semantico**: `controlli → parser → DPB → request-sequencing` (decode).

Scegliere la sonda più economica come primo passo:
- **Se il dump del CAPTURE buffer (Step 4a) è economico** (già si ha il punto di hook in `v4l2vk_vk_device.c`): dump (a) PER PRIMO.
  - (a) frame **plausibile** → il decode-side è ~ok → **salta temporaneamente** H-CONTROL/H-DPB → indaga COPY2/`vulkandownload` (Step 4 b/c) e H-GST-NEGO (Step 5).
  - (a) **blank** → bug sul decode-side → vai a Step 1 (tracer) / Step 3 (parser).
- **Se ricostruire l'ICD è più costoso del tracer** (probabile): **Step 1 (tracer) per primo** — non richiede rebuild.

### Decision tree (ordine operativo)
```
Reproduce (FATTO) → Precond 0 + Precond 1 → §5 ref-stream+corpus
  → Step 0 (fast boundary probe: scegli entry)
       ramo readback-first:
         (a) plausibile → COPY2/vulkandownload (Step 4 b/c) → H-GST-NEGO (Step 5)
         (a) blank      → decode-side: Step 1
       ramo tracer-first:
         Step 1 (tracer control-diff)
            mismatch → H-CONTROL/H-PARSER → [Step 3 parser-verify se serve isolare parser vs mapping] → fix
            match    → Step 2 (DPB binding & sequencing + per-request)
                          fail → H-DPB → fix
                          ok   → Step 4 (readback bisection a/b/c)
                                   (a)blank        → torna decode-side (Step 1/2/3)
                                   (a)ok+(b)blank  → H-FORMAT/COPY2 → fix (checklist §6.A)
                                   (b)ok+(c)blank  → H-GST-NEGO → Step 5
  → Verify (criteri §1) → [Scope checkpoint se root-cause profonda]
```

### Step 1 — v4l2-tracer control diff (H-CONTROL/H-PARSER, localizzazione rapida)
```
# ours
v4l2-tracer -u trace gst-launch-1.0 filesrc location=test.h264 ! h264parse config-interval=1 ! \
  video/x-h264,alignment=au,stream-format=byte-stream ! identity eos-after=1 ! vulkanh264dec ! fakesink sync=false
# golden
v4l2-tracer -u trace gst-launch-1.0 filesrc location=test.h264 ! h264parse config-interval=1 ! \
  video/x-h264,alignment=au,stream-format=byte-stream ! identity eos-after=1 ! v4l2slh264dec ! fakesink sync=false
```
- `config-interval=1` → SPS/PPS in ogni AU anche da container (F-03). `alignment=au` → 1 buffer = 1 AU.
- `identity eos-after=1` → EOS dopo **1 AU** (NON usare `filesrc num-buffers=1`: conta i buffer di `filesrc`, non gli AU — P0.2). **Verificare** che `eos-after=1` produca davvero 1 AU sul nostro stream; **fallback**: tracciare lo stream intero e filtrare offline il primo `DECODE_PARAMS` utile.
- Confronta VALORI: `SPS.level_idc` (raw 31 non enum 8), `DECODE_PARAMS.flags` (PFRAME/BFRAME), `dpb[].reference_ts`, timestamp QBUF, presenza SCALING_MATRIX. Match → NON dichiarare "decode ok": vai a Step 2.

### Step 2 — DPB binding & sequencing + per-request (H-DPB, H-CONTROL/P1.8)
Verifica le osservabili del criterio §1.2 (reference_ts→buffer risolto, ref-count ≤ num_ref_frames, unità timestamp coerenti — R9). Anche con SPS/PPS/DECODE_PARAMS identici l'output può essere blank se il binding o il sequencing request è errato.
- **Per-request V4L2 stateless** (P1.8): (i) i controlli sono associati al `request_fd` corretto; (ii) ordine `S_EXT_CTRLS(request) → QBUF OUTPUT(request) → MEDIA_REQUEST_IOC_QUEUE`; (iii) un request per frame/AU (salvo multi-slice); (iv) nessun riuso prematuro di request/buffer prima del DQBUF/completion; (v) multi-slice → tutte le slice rappresentate.

### Step 3 — Verifica parser (H-PARSER, condizionale: solo se Step 1/2 non localizzano)
Dump dei controlli raw che l'ICD passa a V4L2 (SPS/PPS/slice-params) e confronto con un **parser indipendente** (`h264bitstream`/`ffmpeg -debug`) — NON `v4l2slh264dec` (condivide il parser GStreamer `h264parse` → non isola il parser dell'ICD). Confronto mirato sui campi first-class di H-PARSER (`pic_order_cnt_bit_size`, `dec_ref_pic_marking_bit_size`, MMCO, `frame_num`, `pic_order_cnt_lsb`, `idr_pic_id`, flag IDR/reference/field). Match → è il mapping; mismatch → bug nel parser stopgap.

### Step 4 — Bisezione readback a 3 confini (H-FORMAT, readback-first)
- **(a) V4L2 CAPTURE buffer** (mmap): dump dopo `VIDIOC_DQBUF CAPTURE`, PRIMA di COPY2. Target: `src/vulkan-v4l2/v4l2vk_vk_device.c`, in `QueueSubmit2`/submit-job dopo lo step DQBUF; `capture_bufs[dq_cap].mmap_addr`. **R11 (cache/DMA sync)**: il buffer dev'essere DQBUF-ato (non in-flight); registrare `bytesused`; se DMABUF-backed eseguire `DMA_BUF_IOCTL_SYNC` (START prima della lettura CPU, END dopo) per coerenza cache ARM — senza sync si rischia stale/blank → **falso H-FORMAT/decode-side**; salvare timestamp + buffer-index. Vedi anche logging multi-planar §6.A.
- **(b) VkImage host-mem dell'ICD**: dump dopo COPY2, PRIMA che `vulkandownload` legga (ptr `posix_memalign`).
- **(c) output `vulkandownload`**: già disponibile (file blank attuale).
- Localizza: (a)blank → decode-side. (a)ok+(b)blank → COPY2 (stride/offset/format → checklist §6.A). (b)ok+(c)blank → `vulkandownload` (rowPitch/format-props/layout) → Step 5.

### Step 5 — GStreamer negoziazione (H-GST-NEGO, indipendente da a/b/c)
```
GST_DEBUG=vulkanh264dec:9,vulkandownload:9,vulkandeviceprovider:5 gst-launch-1.0 ...
```
Logga capability scelte (profilo/formato/DPB-mode/memory) vs quelle esposte dall'ICD. ICD risponde male a una query → path blank a monte. (Programma Vulkan diretto come validazione = **out-of-scope B0**: se H-GST-NEGO blocca GStreamer, è input per Stage-2, vedi R7.)

### §6.A — Diagnose H-FORMAT (checklist formato/modifier rkvdec + logging multi-planar)
1. `VIDIOC_G_FMT` su `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE` → `pixelformat`, `width`, `height`, `num_planes`, e **per OGNI piano** `plane_fmt[i].bytesperline` + `plane_fmt[i].sizeimage`.
2. `VIDIOC_ENUM_FMT` → flag `V4L2_FMT_FLAG_COMPRESSED`/`_EMULATED` (annotare).
3. **Pitch effettivo (primario)**: inferire da `bytesperline` (Y) e `sizeimage`; confronto col lineare assunto `ALIGN(w,256)`. **(Secondario, se inconcludente)**: `VIDIOC_EXPBUF` → dma-buf fd → query DRM modifier (~30 righe C / tool dedicato — solo se l'aritmetica non basta).
4. Controlli che influenzano il layout (es. `V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY`).
5. Verdetto linear vs Rockchip-tiled, **dimostrato coi valori misurati**. Se tiled + nessuna conversione semplice → Stage-2. **(Chiude open-Q Gate-2.)**
- **Logging multi-planar (P1.6, deliverable)**: NON limitarsi a `plane_fmt[0]`. Registrare `num_planes` + per-piano `bytesperline`, `sizeimage`, `bytesused`, `length`, `data_offset`, `mmap addr`; **layout interpretato**: NV12 single-plane / NV12M (2 piani) / tiled / unknown.

### §6.B — Stride-normalization (operativa; coded vs visible P0.3, UV-row P1.5)
- **coded size** `Wc×Hc`: da SPS (`pic_width_in_mbs_minus1`, `pic_height_in_map_units_minus1`) → MB-aligned (multipli di 16). È la dimensione del buffer reconstructed, **prima** del display crop.
- **visible size** `Wv×Hv`: coded meno `frame_crop_{left,right,top,bottom}_offset` (se `frame_cropping_flag`). È ciò che ffmpeg emette.
- **comparison size** = **visible** (`Wv×Hv`). Confrontare a coded **falsa** il match (ffmpeg già croppa → P0.3).
- **stride reali**: leggere `stride_y` e `stride_uv` dalla metadata V4L2 reale (`bytesperline` per piano, §6.A). **Non assumere** `stride_uv == stride_y` (spesso uguali, non garantito — P1.5).
- **normalizzazione** (identica a `ours` e `ref`) → **2 artifact**:
  - **coded-normalized** (diagnostico): luma `y∈[0,Hc)` primi `Wc` byte da `stride_y`; UV `y∈[0,Hc/2)` primi `Wc` byte da `stride_uv`.
  - **visible-normalized** (GATE, §1.3): luma `y∈[0,Hv)` primi `Wv` byte da `stride_y`; UV `y∈[0,Hv/2)` primi `Wv` byte da `stride_uv`. **Riga chroma NV12 = `Wv` byte** (= `Wv/2` coppie CbCr interleaved), **NON `Wv/2` byte** (P1.5). Scartare padding destra/sotto.
- **dump**: sostituire `fakesink` con `filesink location=our.nv12`. Riferimento: `ffmpeg -i test.h264 -f rawvideo -pix_fmt nv12 ref.nv12`.
- **differenza**: byte-exact su visible-normalized (PRIMARIO); `PSNR = 10·log10(255²/MSE)` (fallback, §1.3).

**Fix** → registra patch in `compat-mesa26.patch` (+ flag mismatch uAPI kernel = finding). **Verify**: criteri §1.

**Scope checkpoint**: se root-cause = (i) bug kernel rkvdec, o (ii) codice ICD sostanziale nuovo (gestione tiled oltre uno stride-fix) → STOP. **Output artifact**: entry in §8 con status BLOCKED + root-cause; il finding sale al gate Stage-2. No B0 open-ended.

---

## 7. Non-goals (YAGNI)

zero-copy DMA-BUF (CPU-copy basta per correttezza → B1); build/integrazione Chromium (Stage-2/C); HEVC/VP9/AV1; performance/jitter; contribuzione upstream; far PASSARE VK-CTS (solo stretch); **programma Vulkan diretto** (solo se H-GST-NEGO si rivela bloccante → Stage-2).

---

## 8. Deliverable

- ICD corretto (`.so` + `compat-mesa26.patch` aggiornato).
- **`artifacts/phase-b0/`** (directory riproducibile):
  - `input/` — stream H264 + SHA256.
  - `traces/` — v4l2-tracer `ours` + `golden`.
  - `dumps/` — readback (a)/(b)/(c).
  - `metadata/` — Precond-1 driver identity, `G_FMT` per-plane, strides.
  - `compare/` — normalized (coded + visible) + diff/PSNR.
  - **Per ogni dump**: SHA256 + frame-index + PTS + buffer-index + `pixelformat` + `w`/`h` + visible-crop + strides(y,uv) + `num_planes` + `bytesused`.
- Vault: `analyses/phase-b0-h264-correctness-findings.md` — root cause (ipotesi/confine), fix, risultati v4l2-tracer + DPB-binding + confronto byte-exact/PSNR visible-normalized, esito VK-CTS, input Stage-2, e **inventario formato/modifier rkvdec** con campi: `pixelformat` (hex fourcc), `width`/`height`, per-plane `bytesperline`+`sizeimage`, `num_planes`, DRM modifier (o "non esposto"), **tiled vs linear**.
- gap-tracker: **Gate 5** (mapping/decode H264) → CONFERMATO sul nostro build se fixato; **Gate 2** (format-modifier) risolto.

---

## 9. Stage-2 (architecture gate) — GATED

- **Trigger**: correttezza Stage-1 raggiunta.
- **"Parallelo" = SOLO lettura documentale** (design-doc; **CL su Chromium Gerrit dir `media/gpu/vulkan/`** via gerrit-REST — fattibile ora: Chromium controlla l'identità del device / richiede single `VkPhysicalDevice`?; inventario format-modifier da Stage-1; traiettoria extension non-parsing). **NESSUNA decisione architetturale finale finché Stage-1 non è chiuso.**
- **Decisione** (post Stage-1): standalone-ICD vs estendere-PanVK vs DMA-BUF-interop → ADR aggiornato + scelta Fase B. Spec propria.

---

## 10. Rischi

- **R1** VK-CTS infattibile sullo stub → stretch-only; gating = tracer + DPB + byte-exact visible-compare.
- **R2** root-cause = bug kernel rkvdec → investigazione kernel gated (scope checkpoint).
- **R3** format-modifier tiled + PanVK-incompatibile → informa Stage-2, non blocca Stage-1 (readback CPU-copy lineare può dare output GStreamer corretto).
- **R4** **SBC IRRAGGIUNGIBILE** (down 2026-06-18). Coperta da **Precondizione 0** (§4): SBC reachable + tool gating + sample H264 prima di ogni step.
- **R5** golden `v4l2slh264dec` ha differenze valide (DPB/SLICE mgmt, color-convert, stride-padding) → confrontare VALORI controlli + output **visible-normalized**, NON struttura grezza né MD5 naive.
- **R6** **ABI drift V4L2 stateless** kernel-7.1rc1 vs prototipo sree: oltre al `QUERY_EXT_CTRL` EINVAL (atteso), struct `v4l2_ctrl_h264_sps/pps/decode_params` potrebbero avere layout/flag cambiati → dump raw struct + confronto con header del kernel del branch sree.
- **R7** `vulkanh264dec` non supporta il modello memoria/profilo dell'ICD (DPB separate vs combined, `video_maintenance1` mancante) → GST_DEBUG (Step 5); se GStreamer è falso-negativo, il programma Vulkan diretto è **out-of-scope B0** → input Stage-2.
- **R8** Vulkan loader/ICD manifest incompatibile col pin mesa 26.0.6 → `vulkaninfo` con `VK_LOADER_DEBUG=all` DURANTE B0.
- **R9** timestamp mismatch OUTPUT↔CAPTURE → tracciare tutti i timestamp, verificare che i reference puntino a timestamp DQBUF-ati (Step 2).
- **R10** sample diverso dal riferimento → confronto invalido. Stesso file (SHA256, §5) o golden rigenerato sullo stesso SBC.
- **R11** **cache/DMA coherency**: dump del CAPTURE buffer senza sync → dati stale/blank → **falso H-FORMAT/decode-side**. Prima del dump: buffer DQBUF-ato, registrare `bytesused`, `DMA_BUF_IOCTL_SYNC` (START/END) se DMABUF-backed, salvare timestamp + index (Step 4a).

---

## 11. Conformità protocolli

- **systematic-debugging** (reproduce→isolate→diagnose→fix→verify) — metodo §6 con fast-boundary-probe (Step 0) + decision-tree.
- **TDD-where-applicable**: harness di verifica (tracer + DPB-check + byte-exact/PSNR visible-compare) prima/insieme al fix.
- **Tool selection**: `serena`/`kread` per il C nel volume; Docker build; SBC test. No grep brute sul tree.
- **verification-before-completion**: criteri §1 = gate; nessun claim "corretto" senza evidenza.
- **Vault writes**: seguono la governance del vault — `OBSIDIAN_Kernel/VulkanVideo/CLAUDE.md` + la regola di provenienza (`vulkanvideo-rk3588-agent/agent/VERIFICATION.md §0-ter`): provenance self-check (ogni `Source-*` citato esiste), status solo da body letto, frontmatter YAML, confidence H/M/L.
- **Anti-allucinazione**: Gate 5 = MEDIUM-sul-nostro-build finché non riprodotto.
- **Contribution gate chiuso**; outbound = decisione gated separata.
