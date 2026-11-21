# GPU time on macOS: Metal System Trace with Vulkan labels

KosmicKrisp's Vulkan timestamps are unreliable for per-pass timing (zeros, a timestamp reused per
render stage), so `SPARTAN_GPU_PROFILE=1` gives no usable numbers there. Instead:

1. Run the game with the graphics service and
   `KK_LABEL_ENCODERS=1 SPARTAN_GPU_PROFILE=labels`: the video core inserts a debug label at
   every render pass (`pass WxH [targets] ... vs ps`) and dispatch (`dispatch cs <hash>`, and
   `internal: ...` for its own compute: tile manager, FSR, HLE copies); KosmicKrisp with
   `packaging/macos/kosmickrisp-ps4mac-diagnostics.patch` names the Metal encoder after it, or after
   the function that opened a compute encoder (`kk: ...`).
2. Record the service:
   `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcrun xctrace record --template 'Metal System Trace' --attach <pid> --time-limit 4s --output run.trace`
   (do not interrupt it while it saves).
3. Export and sum:
   `xcrun xctrace export --input run.trace --xpath '/trace-toc/run/data/table[@schema="metal-gpu-intervals"]' > run.xml`,
   then `gpu_busy.py run.xml ps4mac_service` (how much of the time the GPU is busy, by channel;
   intervals unioned) and `gpu_labels.py run.xml <fps>` (ms per frame by label).

Limits: the "GPU Execution" rows of the trace carry no label and their encoder ids do not match
the encoders, so only part of the time gets a name.

## Metal frame capture (counters, limiters, shader cost by line)

KosmicKrisp's own capture (`MESA_KK_GPU_CAPTURE`) records the whole session; the ps4mac patch
adds a triggered one: `MTL_CAPTURE_ENABLED=1 KK_CAPTURE_TRIGGER=<file>
KK_CAPTURE_DIRECTORY=<dir> KK_CAPTURE_SUBMITS=<n>` on the graphics service; creating `<file>`
captures the next n queue submissions (R&C: ~19 a frame) to `<dir>/ps4mac_service.gputrace`.
`capture_rnc_level.sh [submissions]` (from RnC/autotest) does it in the R&C level.

Size: Metal stores every resource the work references, guest memory included — 20 submissions
are ~7.7 GB, 60 are ~18 GB. Open the `.gputrace` in Xcode (File > Open) on this Mac; it replays
the frame on the GPU. Useful views: Summary > Performance (time per encoder), the Counters view
(memory bandwidth, limiters such as ALU / texture / memory per encoder), and a shader's Profile
(cost per line).
