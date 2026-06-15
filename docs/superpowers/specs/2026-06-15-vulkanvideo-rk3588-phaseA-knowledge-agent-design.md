# Spec — Fase A: RK3588 Vulkan Video Decode — Knowledge Agent (bleeding-edge)

- **Data**: 2026-06-15 (rev. 2, post code-review)
- **Status**: Fase A APPROVATA per implementazione. Fase B GATED su B0 (vedi §8).
- **Scope**: SOLO Fase A. B0/B/C avranno spec proprie.
- **Lingua**: IT con handle/termini tecnici EN. Vault prodotto = EN.

---

## 0. Contesto + decomposizione

**Premessa strategica**: browser HW video decode su RK3588 bloccato. VA-API = abbandonata (libva-v4l2-request morto dal 2019, Mesa non espone VA-API per Rockchip — `firefox-vaapi-investigation-20260427`). V4L2-stateless lato browser = muro Chromium + artefatti VP9 ricorrenti. Direzione industriale emergente = **Vulkan Video** come backend decode cross-platform (Chromium ci sta migrando; Khronos+NVIDIA driving — *claims UNVERIFIED, §10*).

**Ipotesi di progetto** (da FALSIFICARE in B0, NON certezza): il pezzo mancante per RK3588 ≈ **driver Vulkan Video decode V4L2-backed** (pilota il VPU rkvdec, non la Mali GPU). Potrebbe rivelarsi più specifico: integrazione Vulkan Video *dentro lo stesso device* usato da Chromium; oppure un path DMA-BUF/modifier/sync compatibile rkvdec↔PanVK; oppure un backend Chromium più che un driver Mesa generico. B0 serve proprio a discriminare tra queste.

**Decomposizione (4 sub-progetti, ognuno spec→plan→build)**:
- **A** (questo doc) — Knowledge system bleeding-edge + frontier-tracking. Read/track-only, **no codice**. Inquadra i gate P0, progetta B0, produce readiness.
- **B0** — **Feasibility spike** (codice minimo throwaway). Prova le decisioni dure (device/memory/sync model, 1 frame H264 decode). Gate bloccante prima di B.
- **B** — Primo pezzo del driver reale (1 codec, H264), informato da B0.
- **C** — Driver RK3588 Vulkan Video decode completo, browser-ready.

**Insight chiave (da review)**: il rischio del progetto NON è "scrivere il codec" — è **architettura Vulkan/V4L2/memoria/browser**. A+B0 esistono per de-rischiare *questo*.

---

## 1. Obiettivo + criteri di successo (Fase A)

**Obiettivo**: motore che raccoglie, *verifica* e struttura la documentazione bleeding-edge + attività upstream necessarie a (1) decidere l'architettura del driver, (2) progettare lo spike B0, (3) alimentare B/C. Ci rende e ci mantiene i più aggiornati del dominio.

**Criteri di successo (misurabili)**:
- Coverage: sorgenti Tier-1/2 "continuo" tracciate, staleness ≤ 7 giorni.
- Gap-tracker: open-question verso B0-readiness in calo monotòno.
- Test: dal vault rispondiamo con citazioni **primarie** a "struttura di un driver Vulkan Video decode" e "cosa serve a un backend V4L2".
- Milestone: **Phase-B0 Readiness Assessment** (§7) con architettura decisa + B0-plan eseguibile.

**Non è**: codice driver (è B0/B). Non è news-agent generico (è goal-directed).

---

## 2. Source registry (tiered × intensità)

Manifest: `handle`, `fetch_method`, `intensity`, `cursor`.

**Intensità**: `continuo` (ogni run) / `reference` (deep-study one-shot, poi dormiente) / `signal` (scan settimanale).

### TIER 1 — Spec (Khronos) [continuo]
- `github.com/KhronosGroup/Vulkan-Docs` → `VK_KHR_video_decode_{queue,h264,h265,av1,vp9}` (vp9 provisional, §10) + issue #1497.
- `docs.vulkan.org` proposals (modello DPB per-codec).
- ⭐ Khronos "Vulkan Video Integration into Chromium" design doc (khronos.org, v1.0 2025-12-04 Draft — *esistenza MED, contenuto UNVERIFIED §10*). Transitivo → gerrit CL Chromium.
- `KhronosGroup/VK-GL-CTS` (video decode tests, per-codec coverage H264/H265/AV1/VP9) + Vulkan-Samples video → conformance reference.
- **`KhronosGroup/Vulkan-Loader`** [NUOVO da review] → ICD JSON, device enumeration, layer behavior (decide accettabilità ICD standalone).

### TIER 2 — Mesa (docs + repo + DEV) [continuo + reference]
- Docs: `docs.mesa3d.org` + `docs/features.txt` (supporto Vulkan Video per-driver). [continuo]
- Repo: `gitlab.freedesktop.org/mesa/mesa` `main`. MR-query `merge_requests?search=video` (verificato 2026-06-15: `radv/video`, `anv/video`, `hasvk` MR `!21183`, `radeonsi/mm`, `ac/video`). [continuo, alta su `vk_video` runtime comune]
- Reference impl (deep-study one-shot): Vulkan Video decode in `src/amd/vulkan` (radv) + `src/intel/vulkan` (anv) + `src/vulkan/runtime` (`vk_video` comune). Path **da confermare nel deep-study** (§10). [reference]
- Gap: nessun driver V4L2/panvk Vulkan-Video emerso (LOW/MED, §10 — confermare con repo-tree + issues + mailing list).
- `gitlab.freedesktop.org` issues/`work_items/14987` (bridge V4L2↔Vulkan — **UNVERIFIED, pinnare §10**). [continuo]

### TIER 3 — Chromium (consumer) [continuo]
- gerrit CL dal design-doc + `media/gpu/vulkan` + phase-out VaapiVideoDecoder. `chromium-review.googlesource.com`, `issues.chromium.org`.
- **Chromium media tests** [NUOVO]: `media/gpu` Vulkan video decoder tests + Ozone/Wayland video path.

### TIER 4 — Kernel / V4L2 (backend) [continuo + reference]
- `linux-media` (lore) — bridge V4L2-stateless↔Vulkan (Nicolas/Detlev). [continuo]
- rkvdec uAPI (`docs.kernel.org` V4L2 stateless) + nostro tree rkvdec-vdpu381 + Obsidian `Kernel/`. [reference]
- **`v4l2-compliance` / `v4l2-request-test` (se nel nostro stack) / GStreamer stateless pipelines** [NUOVO] → baseline pratica + verifica stabilità H264 stateless rkvdec.
- **DMA-BUF / DRM format-modifiers docs** [NUOVO]: Linux DMA-BUF docs, DRM modifiers, PanVK/Panthor import/export capabilities → memory model.

### TIER 5 — Attori / segnale [signal]
- NVIDIA dev-forum/blog, Collabora (Nicolas/Detlev), Igalia, Google media; LWN/FOSDEM/blog Mesa-Khronos.

### Nota fetch
- Aperte (Khronos github, Chromium gerrit, lore via search, docs.kernel.org, gzip docs): HTTP headless OK.
- **freedesktop GitLab (Mesa) = Anubis** → token API read-only freedesktop primario (ASSUNZIONE Task-0 §10); fallback browser/CiC. Collabora API = aperta.

---

## 3. Architettura / componenti

Riuso pattern **AI-news-agent**. 

| # | Componente | Riuso/Nuovo | Funzione |
|---|---|---|---|
| 1 | Source registry (manifest) | NUOVO | §2. |
| 2 | Ingestion routine | Riuso harness | Routine cloud schedulata; fetch diff per-source. |
| 3 | Verifier fail-closed | Riuso+adatta `VERIFICATION.md` | §5: body primario, confidence H/M/L, UNVERIFIED, mai metadata. |
| 4 | Knowledge store | Riuso schema, NUOVO vault | `OBSIDIAN_Kernel/VulkanVideo/` (sources/concepts/entities/analyses + log/index/overview). Proprio CLAUDE.md (§9). |
| 5 | Gap-tracker | NUOVO | Doc vivo Phase-B0 readiness: gate P0, decisioni architettura, blocker. |
| 6 | Notifier | Riuso | Telegram (Markdown v1 + fallback). |

**Repo agente**: nuovo `dongioia/vulkanvideo-rk3588-agent` (clona ai-news-agent). Scrive vault via branch `claude/*` → Action merge → obsidian-git pull (clonare `merge-ai-news.yml` per path `VulkanVideo/`).

**Query lato nostro**: filesystem `Read`/`Grep` sul vault. MCP = YAGNI.

---

## 4. Data flow

```
source registry → ingestion (token-API Mesa / HTTP aperte / CiC fallback)
  → verifier fail-closed → write vault (branch→Action→pull)
  → update gap-tracker (B0 readiness) → Telegram digest
```

---

## 5. Verification (fail-closed + completeness)

Binding `feedback_research_must_read_sources`: leggere **body** primario, mai listing/snippet/metadata; confidence H/M/L; fonte non leggibile → UNVERIFIED; snippet motore ≠ sufficiente per write. Riuso anti-injection di `VERIFICATION.md`.

---

## 6. Cadenza operativa

- **Kickoff one-shot**: deep-study reference (radv + anv + `vk_video`) → analisi-template fondante; risolve "esiste driver virtuale/non-GPU-bound?" (§13).
- **Continuo** 2×/sett (mar+ven); **Signal** 1×/sett (sab); **Event-triggered** (tag Mesa, version-bump design-doc, reply patch nostra).
- **Vincolo**: cap routine cloud ~15/giorno account-level (con ai-news-agent) → OK; no spike event non-batchati.

**Priorità kickoff** (alcune domande dominano le altre):
1. vincolo Chromium same-device/separate-device;
2. device model Mesa/Vulkan feasibility;
3. memory / DMA-BUF / format-modifier path;
4. sync model;
5. mapping H264 Vulkan Video → V4L2;
6. deep-study reference implementation;
7. VP9 status.

VP9 conta per C/browser-completeness ma **NON blocca B0 H264**.

---

## 7. Gate tecnici P0 (A inquadra, B0 prova)

A **non** risolve questi da sola documentazione — li *inquadra* con evidenza primaria/reference, e **progetta l'esperimento B0** che li prova. Ogni gate ha: stato docs (cosa dicono le fonti) + esperimento B0 + criterio di risposta.

### 7.1 Device model (decisione più rischiosa del progetto)
- Il decoder Vulkan Video deve stare sullo **stesso `VkPhysicalDevice`** del rendering/compositing che Chromium usa? Se sì, un ICD video-only separato può essere inutile/difficile.
- Un **ICD standalone video-only** è accettabile per Chromium?
- **Mesa** accetterebbe un driver Vulkan Video V4L2-backed non legato a una GPU? (domanda upstream, non solo tecnica)

### 7.2 Memory / interoperability model (nodo tecnico più critico)
- Chi alloca le immagini di output decode? (`VkImage` per DPB/output vs V4L2 capture buffers)
- Il backend V4L2 può decodificare direttamente in memoria rappresentata come `VkImage`?
- Quelle immagini sono export/import come **DMA-BUF**?
- Formati/modifiers `rkvdec` RK3588 (NV12/P010, linear/tiled, Rockchip-specific) compatibili con sampling PanVK/Chromium?
- Cache coherency; allocator da DMA-heap/GBM.

### 7.3 Synchronization model
- Mapping completion **V4L2 request** ↔ **fences/timeline semaphores Vulkan**; explicit sync; queue ownership.

### 7.4 Chromium integration constraints ("browser-ready" = vincolo duro)
- Chromium accetta video decode da un Vulkan device **diverso** da quello di rendering?
- Il design atteso usa `VkVideoQueue` sullo stesso `VkDevice`?
- Extensions Vulkan richieste oltre le video-decode?
- Path Linux/Ozone/Wayland effettivo; pixel formats accettati; protected content / color space / HDR / 10-bit.

### 7.5 Codec mapping minimo (H264)
- Strutture Vulkan Video H264 → controlli V4L2 stateless; DPB ownership; bitstream input handling.
- Stabilità del path H264 stateless V4L2 su RK3588/rkvdec (controlli necessari, qualità supporto kernel).

### Codec selection rationale (H264 first)
Target iniziale: **H264 decode**. Motivi/ipotesi **da validare**: estensione Vulkan Video H264 più matura di VP9; CTS/reference disponibili; rilevanza browser; supporto V4L2 stateless RK3588 **presumibilmente** più maturo degli altri codec (claim MED/UNVERIFIED, §10); minore complessità DPB/bitstream di VP9/AV1.
**Exit condition B0**: decodificare uno stream di conformance H264 via **Vulkan Video API backed-by-V4L2 request API**.

---

## 7-bis. B0 Feasibility Spike — requisiti

A deve produrre un **B0 plan** che risponda con prototipo minimo:
1. **Vulkan device model**: same-device vs ICD separato; aspettative Chromium; accettabilità upstream Mesa.
2. **Memory model**: strategia alloc `VkImage`; export/import DMA-BUF; ownership V4L2 capture buffer; formati/modifiers RK3588; path zero-copy verso rendering.
3. **Sync model**: completion V4L2 request; fences/semaphores; explicit sync.
4. **Codec mapping minimo**: strutture H264 Vulkan → controlli V4L2 stateless; DPB; bitstream input.
5. **Test target** (concreto, verificabile):
   - ICD JSON installabile localmente;
   - `vulkaninfo` vede il device + capabilities video;
   - `vkGetPhysicalDeviceVideoCapabilitiesKHR` risponde coerentemente (H264);
   - `vkCreateVideoSessionKHR` riesce per H264;
   - decode di 1 stream/frame H264 di conformance;
   - dump/output frame verificabile;
   - test DMA-BUF export/import separato (o motivazione del perché non richiesto).

---

## 7-ter. Deliverable Fase A (artefatti concreti)

Evita note sparse: A produce artefatti nominati nel vault `VulkanVideo/analyses/` + tracker:
- `analyses/mesa-vulkan-video-driver-anatomy.md` (deep-study radv/anv/`vk_video`)
- `analyses/chromium-vulkan-video-integration-constraints.md` (§7.4)
- `analyses/rk3588-v4l2-rkvdec-h264-baseline.md` (§7.5, stabilità + controlli)
- `analyses/memory-dmabuf-vkimage-model.md` (§7.2)
- `analyses/sync-v4l2-vulkan-model.md` (§7.3)
- `analyses/architecture-decision-record.md` (scelta tra le 4 opzioni §8 + rationale + upstream-plausibility)
- `analyses/phase-b0-readiness-assessment.md` (sintesi + B0-plan §7-bis)
- `gap-tracker.md` (vivo, open-question + confidence)

---

## 8. Architetture candidate + Gate

### Opzioni (la decisione architetturale è il rischio #1)

| Opz | Approccio | Pro | Contro | Valutazione |
|---|---|---|---|---|
| 1 | **Estendere PanVK** con Vulkan Video V4L2-backed | unico `VkPhysicalDevice` GPU+video; più compatibile Chromium; integrazione immagini/rendering | architetturalmente sporco (PanVK=GPU, VPU=V4L2); upstreamability incerta; sync cross-subsystem; codice RK3588-specific dentro driver Mali | attraente per browser, **upstream-risk alto** |
| 2 | **Nuovo driver Mesa Vulkan video-only/generic V4L2** | pulito; riusabile altri SoC V4L2-stateless; separazione chiara | Chromium può non usarlo se decode/render stesso device; interop PanVK via DMA-BUF/sync = requisito duro; serve giustificazione upstream forte | **miglior target ricerca/prototipo, validare con Chromium** |
| 3 | **ICD standalone fuori Mesa** | prototipo rapido; meno frizione upstream; valida mapping Vulkan→V4L2 | browser-readiness incerta; manutenzione separata; conformance/loader onerosi; dead-end se Chromium richiede Mesa | **buono per B0, non per C** |
| 4 | **Libreria/translation-layer non-ICD** | test harness; riusa V4L2 stateless; valida codec mapping | non risolve Chromium Vulkan Video; non è driver reale | **solo strumento interno** |

### Gate A → B0
Parte B0 quando:
- i gate P0 §7 sono inquadrati con evidenza primaria **o marcati esplicitamente come gap**;
- il B0-plan §7-bis è **prodotto ed eseguibile**;
- ≥1 reference impl Mesa Vulkan Video decode studiata deeply;
- esiste una **decisione architetturale candidata** con upstream-plausibility documentata;
- i vincoli Chromium same-device/separate-device sono **noti o trasformati in esperimento B0**;
- memory/sync model hanno almeno un **disegno sperimentale**;
- contribution-protocol non-kernel mappato **prima di qualunque outbound pubblico** (NON bloccante per B0 locale read-only/throwaway).

### Gate B0 → B
Parte B **solo quando** B0 ha **dimostrato o falsificato in modo conclusivo**:
- architettura driver scelta con path upstream plausibile;
- vincolo Chromium same-device/separate-device noto;
- ICD/device enumeration funzionante nel modello scelto;
- capabilities Vulkan Video H264 esposte coerentemente;
- `vkCreateVideoSessionKHR` funzionante;
- ≥1 frame/stream H264 decodificato via V4L2 request API;
- memory model documentato: `VkImage`, V4L2 capture buffers, DMA-BUF, format/modifiers;
- sync model documentato: V4L2 request completion, fences/semaphores/explicit sync;
- test minimo export/import/render **o** motivazione documentata del perché non richiesto per B;
- contribution-protocol Mesa/Chromium/Khronos pronto per eventuale upstream/RFC.

---

## 9. Conformità protocolli

- **AI-news gotchas**: cap ~15/giorno (OK); **network allowlist** estesa (khronos.org, gitlab.freedesktop.org, chromium-review/issues.chromium.org, lore, docs.*); Telegram Markdown v1 + fallback; setup script cwd self-contained; `gh repo create` guard.
- **Vault governance**: `VulkanVideo/` con proprio CLAUDE.md (frontmatter YAML, wiki EN, naming strict, log.md append-only, `[[wiki-link]]`).
- **Completeness rule**: §5, binding nel verifier.
- **Secrets**: token freedesktop in store sicuro (env/connector, non committato). Creato dall'utente.
- **Tool selection**: deep-study radv/anv → serena/code tools, non grep.
- **Commit/branch**: Rock5bPlus su `main` → spec/commit su branch; commit solo su richiesta.

### Gap protocollo NON-kernel (vincolo B0/B/C)
Hard-rule public-contribution = kernel/LKML-centrici. Venue diversi:
- **Chromium**: firma **Google CLA** pre-CL (bloccante C lato-browser).
- **Mesa**: DCO `Signed-off-by` + MR GitLab.
- **Khronos**: spec = IP/membership; CTS = Apache-2.0 + CLA.
- AI-disclosure per-venue da verificare; humanizer sempre.

**Fase A = read/track-only**. Outbound (commento thread, issue, RFC) → gate humanizer + disclosure venue. **Gap-tracker P0**: estendere contribution-protocol a Mesa/Chromium/Khronos PRIMA di B0-outbound.

---

## 10. Claims confidence-graded (fail-closed corretto post-review)

| Claim | Confidence | Fonte | Azione |
|---|---|---|---|
| `VK_KHR_video_decode_vp9` esiste | HIGH | docs.vulkan.org (body letto) | — |
| VP9 = provisional, non ratificato | MED | issue #1497 (snippet) | confermare body in kickoff |
| Khronos design-doc Chromium esiste (2025-12-04 Draft) | **MED (esistenza)** | google result | **body UNVERIFIED → leggere (kickoff/B0-plan)** |
| Contenuto/architettura del design-doc | **UNVERIFIED** | non letto | task kickoff |
| Mesa Vulkan Video dev attivo (radv/anv/hasvk !21183) | HIGH | MR JSON parsato | — |
| Mesa 26.0: panvk no video decode | HIGH | relnotes (body) | — |
| Chromium phase-out VaapiVideoDecoder + NVIDIA driving | **UNVERIFIED** | google AI-overview/snippet | **leggere design-doc/CL/forum primari** |
| Nicolas PoC V4L2-sotto-Vulkan + work_items/14987 | **UNVERIFIED** | memoria 20/05 | **pinnare work_item + body (P0 kickoff)** |
| Nessun V4L2/panvk Vulkan-Video in Mesa | **LOW/MED** | assenza MR-scan | confermare repo-tree + issues + mailing list |
| radv/anv = principali reference impl | MED | inferenza | confermare (NVK?) nel deep-study |
| Path file Mesa (`vk_video.*`, `radv_video.c`) | MED | conoscenza | confermare nel deep-study |
| H264 stateless RK3588/rkvdec = codec più maturo per B0 | MED/UNVERIFIED | tree locale + GStreamer/V4L2 tests da eseguire | validare in kickoff |
| **Token freedesktop bypassa Anubis headless** | **ASSUNZIONE** | non testata | ⚠️ **Task-0** validare; fallisce → semi-locale |
| Chromium Google CLA / Mesa DCO | MED-HIGH | general knowledge | verificare policy corrente al momento B0/B |

**Regola severa (da review)**: ogni fonte google/snippet/AI-overview/memoria = UNVERIFIED finché body primario non letto; assenze da search = max LOW/MED; claim architetturali richiedono link primario (issue/design-doc/codice/mailing list).

**Task-0**: validare token API freedesktop vs Anubis → decide cloud-puro vs semi-locale.

---

## 11. Scope guard (YAGNI) + non-goals

- A NON scrive codice (B0/B). NO RAG/MCP custom. NO monitor HW-register AMD/Intel (solo `vk_video` runtime + signal). NO output pubblico in A. NO nuovo stack ingestione (riuso ai-news).

---

## 12. Rischi

- **R1** Token non bypassa Anubis → semi-locale (mitig: Task-0; fallback CiC).
- **R2** Direzione Vulkan-Video-Chromium si raffredda (Draft Dic-2025) → tesi indebolita (mitig: A traccia proprio questo; gate su convergenza).
- **R3** Greenfield Mesa V4L2-Vulkan = effort scala-Collabora (mitig: A+B0 readiness; B minimale H264).
- **R4** Cap routine cloud condiviso (mitig: cadenza 2×/sett, batch).
- **R5** Contribution-protocol non-kernel non pronto (mitig: gap-tracker P0).
- **R6** [review] **Device model incompatibile Chromium**: driver funziona ma Chromium non lo usa (decode non sul device di rendering) → prima domanda kickoff/B0.
- **R7** [review] **Zero-copy non raggiungibile/instabile**: serve copia CPU/GPU → path inutile per browser → memory-model P0 + test DMA-BUF/import early in B0.
- **R8** [review] **Format/modifier mismatch VPU↔GPU**: rkvdec produce formato non campionabile da PanVK/Chromium → inventario formati RK3588 + test import/render.
- **R9** [review] **Upstream rejection**: Mesa rifiuta driver Vulkan Video generico V4L2 o V4L2 dentro PanVK → early RFC/discussione (post humanizer/disclosure).
- **R10** [review] **CTS passa ma browser fallisce**: conforme a test base ma non al pipeline Chromium → test Chromium-integration già in B, non solo C.

---

## 13. Open questions (kickoff/B0-plan)

- Esiste un driver Vulkan Video "virtuale"/non-GPU-bound in Mesa (template più vicino al backend-esterno)?
- `work_items/14987`: effort attivo da joinare o dormiente?
- Design-doc Khronos-Chromium: prevede path per decode-engine esterni (non-GPU-integrated)?
- VP9 provisional → timeline ratifica?
- ICD standalone video-only: il loader/Chromium lo enumera+usa accanto al device di rendering?
