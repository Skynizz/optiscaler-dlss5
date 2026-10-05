#include "pch.h"
#include "DlssNrFeature_Vk.h"

#include "DlssNr.h"
#include "DlssNr_ExposureScan.h"


#include <Config.h>
#include <menu/menu_common.h>

#include <imgui/imgui.h>

#include <string>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace DlssNr
{

// The "(?)" marker every control carries, matching the rest of the menu.
static void HelpMarker(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A slider that only writes its value when the handle is released.
//
// Some controls -- intensity, the structure and tone strengths -- are read by the model once, when
// the feature is built, so changing one rebuilds the whole feature. Writing on every pixel of a drag
// meant a rebuild per frame, felt as the picture hitching while you scrub. The slider still tracks
// live under the cursor; only the commit that triggers the rebuild waits for release. Cheap controls
// that are just shader constants (detail, colour, paper white) do not use this -- they can afford to
// apply live.
static bool DeferredSlider(const char* label, CustomOptional<float>* opt, float mn, float mx,
                           float def, const char* fmt = "%.2f")
{
    static std::unordered_map<std::string, float> pending;

    auto it = pending.find(label);
    float value = it != pending.end() ? it->second : opt->value_or_default();
    bool changed = false;

    if (ImGui::SliderFloat(label, &value, mn, mx, fmt))
        pending[label] = value;

    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        auto committed = pending.find(label);

        if (committed != pending.end())
        {
            *opt = std::clamp(committed->second, mn, mx);
            pending.erase(committed);
            changed = true;
        }
    }

    ImGui::SameLine();

    const std::string resetId = std::string("Reset##") + label;
    if (ImGui::SmallButton(resetId.c_str()))
    {
        *opt = def;
        pending.erase(std::string(label));   // drop any in-flight drag so the reset actually sticks
        changed = true;
    }

    return changed;
}

void RenderMenu(Config* config, float menuResScale)
{

    // DLSS Neural Rendering -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader("DLSS Neural Rendering"); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        bool enabled = config->DlssNrEnabled.value_or_default();
        if (ImGui::Checkbox("Enable Neural Rendering", &enabled))
            config->DlssNrEnabled = enabled;

        HelpMarker("Synthesises detail in the upscaler's output, before frame generation sees it."
                       "\n\nNeeds two similarly named files beside OptiScaler, one character apart:"
                       "\n  nvngx_dlssnr.dll       NVIDIA's model (~165 MB) -- you supply it"
                       "\n  nvngx.dll_dlssnr.dll   the forwarder (~13 KB) -- ships in this package"
                       "\nUndocumented and driven directly, so none of this is officially supported.");

        // The toggle can be bound to a key, and nobody would think to look for it under Keybinds
        // unless told. Dimmed, because it is a note rather than a setting.
        ImGui::TextDisabled("Can be toggled with a key -- bind it under Keybinds, \"Neural Rendering\".");

        bool applyModel = config->DlssNrApplyModel.value_or_default();
        if (ImGui::Checkbox("Apply the model", &applyModel))
            config->DlssNrApplyModel = applyModel;

        HelpMarker("Whether the model's edit is applied. Off shows the clean upscaler frame while the"
                       "\npass keeps running -- so with Hold frame (under Compare) you can freeze a"
                       "\nframe and toggle this to see the same frozen frame with and without Neural"
                       "\nRendering. Leave it on for normal use.");

        // Either backend. The two keep separate state, and on a native Vulkan game the D3D12 side
        // is never touched -- so asking only that one reports "waiting for the upscaler" over a pass
        // that is demonstrably running.
        const bool vulkan = DlssNr::IsRunningVk();

        // Turning the pass off does not release the model, so the feature handle stays alive and
        // IsRunning keeps answering yes. Reporting a cost from that was wrong in the way that matters
        // most: the toggle is how anyone A/Bs this, so the one moment the number is read is the one
        // moment it describes the frame before last.
        if (!enabled)
        {
            ImGui::TextDisabled("Off. The model stays loaded, so turning this back on is immediate.");
        }
        else if (!DlssNr::IsRunning() && !vulkan)
        {
            const char* reason = DlssNr::FailureReason();

            if (reason[0] != 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Off for this session: %s.", reason);
                ImGui::SameLine();

                if (ImGui::SmallButton("Retry"))
                    DlssNr::RetryAfterFailure();
            }
            else if (enabled)
                ImGui::TextUnformatted("Waiting for the upscaler to run.");
        }
        else
        {
            // The cost belongs here rather than only in the upscaler's breakdown: that tooltip needs
            // OptiScaler's own upscaler to have run, and with native DLSS passing through there is
            // nothing in it to hang this off.
            // Either backend's timer. They measure the same thing by different means, and only one
            // of them is running.
            const auto ms = vulkan ? DlssNr::LastGpuTimeVk() : DlssNr::LastGpuTime();

            // With "Apply the model" off the pass STILL RUNS (so Hold-frame A/B can toggle its edit on
            // a frozen frame) -- it only outputs the clean frame. So the cost is real, and saying so
            // stops the reading looking like a bug. Enable Neural Rendering off is what zeroes it.
            const char* runSuffix =
                !config->DlssNrApplyModel.value_or_default() ? "  (model running, edit hidden)" : "";

            if (ms.has_value())
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running%s - %.2f ms per frame%s",
                                   vulkan ? " natively on Vulkan" : "", ms.value(), runSuffix);
            else if (vulkan)
                // Measured but not yet read: the first few frames are still in the query ring.
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running natively on Vulkan - %llu frames%s",
                                   DlssNr::FramesVk(), runSuffix);
            else
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Running.%s", runSuffix);

            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("The whole pass: the staging copies and the resolve as well as the"
                                  "\nmodel. Timing only the model would flatter the number."
                                  "\n\nCompare it against the frame time at the bottom of this window to"
                                  "\nsee what it is costing you.");
        }

        ImGui::Spacing();
        ImGui::PushItemWidth(220.0f * menuResScale);

        // Any percentage, rather than a handful of steps somebody chose in advance. The lower bound
        // is 25%: below that the model is working on so little of the picture that its answer no
        // longer survives being enlarged onto it.
        // Applied when the handle is let go, not while it is moving.
        //
        // Every distinct value here is a different working size, and a different working size tears
        // down the scratch textures and rebuilds the model. Writing it on each pixel of a drag meant
        // dozens of rebuilds in a second, which is felt as the whole frame hitching. The slider still
        // reads live; only the commit waits.
        static int pendingScale = -1;

        int scalePercent = pendingScale >= 0
                               ? pendingScale
                               : (int) lroundf(config->DlssNrWorkingScale.value_or_default() * 100.0f);

        if (ImGui::SliderInt("Model resolution", &scalePercent, 25, 200, "%d%%"))
            pendingScale = scalePercent;

        if (ImGui::IsItemDeactivatedAfterEdit() && pendingScale >= 0)
        {
            config->DlssNrWorkingScale = std::clamp(pendingScale, 25, 200) / 100.0f;
            pendingScale = -1;
        }

        if (scalePercent > 100)
            ImGui::TextDisabled("Supersampling %.2fx: the model runs ABOVE native, then\n"
                                "is sampled back down. Experimental, and costly -- time grows with the area.",
                                scalePercent / 100.0f);

        if (scalePercent > 100)
        {
            static const char* dsNames[] = { "FSR1", "Bicubic", "Catmull-Rom", "Lanczos2",
                                             "Lanczos3", "Kaiser2", "Kaiser3", "MAGIC" };
            int ds = (int) config->DlssNrScalingDownscaler.value_or_default();
            if (ds < 0 || ds >= IM_ARRAYSIZE(dsNames))
                ds = (int) Scaler::Lanczos3;

            if (ImGui::Combo("Downscaler (NR)", &ds, dsNames, IM_ARRAYSIZE(dsNames)))
                config->DlssNrScalingDownscaler = (Scaler) ds;

            HelpMarker("The filter that averages the model's above-native answer back to display size --"
                           "\nthis is what turns supersampling into LESS noise rather than more. Sharper"
                           "\nfilters (Lanczos3, Kaiser3) keep the most detail; softer ones (Bicubic,"
                           "\nCatmull-Rom) are gentler on ringing. Independent of the Output Scaling"
                           "\ndownscaler, so the two can differ and run at the same time.");
        }

        HelpMarker("What fraction of the frame the model works at. Cost falls with the square of"
                       "\nthis, so half resolution is roughly a quarter of the time."
                       "\n\nThe frame is never reduced. Only the model's contribution is computed small"
                       "\nand enlarged, so the picture underneath is untouched whatever this says."
                       "\n\nWhat it trades: the shading the model adds is broad and survives enlargement;"
                       "\nthe fine structure it synthesises does not, and softens. Worth having when the"
                       "\npass costs more than you want to pay for the detail it returns."
                       "\n\nThe frame itself stays at full detail whatever this says -- only the"
                       "\nmodel's own work is done small.");

        // Meaningful only when the model runs BELOW the frame's size. At 100% -- and above, where
        // supersampling composites its down-legged answer at native -- the residual collapses to the
        // model's own picture and the two modes are identical, so the control says so by going grey.
        {
            const bool reduced = config->DlssNrWorkingScale.value_or_default() < 0.999f;

            if (!reduced)
                ImGui::BeginDisabled();

            static const char* enlargeNames[] = { "Classic", "Matched residual" };
            int enlarge = config->DlssNrTransfer.value_or_default() == 1 ? 1 : 0;

            if (ImGui::Combo("Enlargement", &enlarge, enlargeNames, IM_ARRAYSIZE(enlargeNames)))
                config->DlssNrTransfer = (uint32_t) enlarge;

            if (!reduced)
                ImGui::EndDisabled();

            HelpMarker("How the model's work is brought back up when it ran below the frame's size."
                       "\n\nClassic composes the model's small picture directly against the full-size"
                       "\nframe. Those two disagree by the shrink's blur as well as by the model's edit,"
                       "\nand the composition cannot tell them apart -- it reads the blur as brightness"
                       "\nthe frame has and the model never saw. The lower the model resolution the"
                       "\nlarger that error, and it is the colour shift that shows up at 50%."
                       "\n\nMatched residual carries up only the model's difference and lays it on the"
                       "\nframe's own proxy, so both pictures being compared are full size and the only"
                       "\nthing that came from the small raster is the edit itself."
                       "\n\nNo effect at 100% or above: there is no residual to carry and the two are"
                       "\nidentical (supersampling brings its answer down to frame size before this)."
                       "\n\nFrom hhkbble's multi-pass work on this fork.");
        }

        // Joint bilateral enlargement. Like Enlargement above, meaningful only below 100%, and greyed
        // otherwise; its one parameter shows only while it is on.
        {
            const bool reduced = config->DlssNrWorkingScale.value_or_default() < 0.999f;
            bool jbu = config->DlssNrJbuUpsample.value_or_default();

            if (!reduced)
                ImGui::BeginDisabled();

            if (ImGui::Checkbox("Edge-aware enlargement", &jbu))
                config->DlssNrJbuUpsample = jbu;

            if (!reduced)
                ImGui::EndDisabled();

            HelpMarker("Brings a below-100% model's work up to full size guided by the full-size frame"
                       "\n(joint bilateral upsampling), instead of a plain bilinear stretch."
                       "\n\nThe stretch reads the four nearest small pixels whatever is in them, so the"
                       "\nmodel's edit smears across every edge the small picture could not resolve --"
                       "\nleaves, hair, a sword against the sky. Here each small pixel only contributes"
                       "\nwhere it looks like the full-size pixel it is landing on."
                       "\n\nNo effect at 100% and above. Off is the resolve exactly as before.");

            if (reduced && jbu)
            {
                float sigma = config->DlssNrJbuSigma.value_or_default();

                if (ImGui::SliderFloat("Edge sensitivity", &sigma, 0.01f, 0.5f, "%.3f", ImGuiSliderFlags_Logarithmic))
                    config->DlssNrJbuSigma = std::clamp(sigma, 0.005f, 1.0f);

                HelpMarker("How different two pixels may look before they stop sharing the model's edit."
                           "\nLower keeps edges crisper; too low and fine texture turns blocky.");
            }
        }

        ImGui::SeparatorText("Benchmark: your technique vs vanilla");

        {
            static bool includeOff = true;
            const auto bs = DlssNr::GetBenchmarkStatus();

            if (!bs.active)
            {
                if (ImGui::Button("Run the FPS comparison"))
                    DlssNr::StartBenchmark(includeOff);

                ImGui::SameLine();
                ImGui::Checkbox("Include NR off", &includeOff);
            }
            else
            {
                ImGui::Text("Measuring: %s %s (%.0f%%)", DlssNr::BenchmarkPhaseName(bs.phase),
                            bs.warmingUp ? "- warming up" : "", 100.0f * bs.phaseProgress);
                ImGui::SameLine();

                if (ImGui::SmallButton("Cancel##bench"))
                    DlssNr::CancelBenchmark();
            }

            HelpMarker("Runs the same scene with Neural Rendering off, then as it ships (the model every"
                       "\nframe), then with the edit cache on your current settings -- 3 s to settle and"
                       "\n8 s measured each. Stand still, or walk the same path each time: what is on"
                       "\nscreen changes the numbers more than anything."
                       "\n\nFrames are the ones the game renders: frame generation multiplies what you see"
                       "\nbut costs nothing here, so it is left out. Results also go to"
                       "\ndlssnr-benchmark.txt beside OptiScaler.");

            const auto& vanilla = bs.results[1];

            if (ImGui::BeginTable("dlssnr-bench", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("Mode");
                ImGui::TableSetupColumn("FPS");
                ImGui::TableSetupColumn("1% low");
                ImGui::TableSetupColumn("NR pass");
                ImGui::TableSetupColumn("vs vanilla");
                ImGui::TableHeadersRow();

                for (int p = 0; p < 3; ++p)
                {
                    const auto& r = bs.results[p];

                    if (!r.valid)
                        continue;

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(DlssNr::BenchmarkPhaseName(p));
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f", r.fps);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f", r.low1);
                    ImGui::TableNextColumn();
                    ImGui::Text(p == 0 ? "-" : "%.2f ms", r.nrMs);
                    ImGui::TableNextColumn();

                    if (vanilla.valid && p != 1 && vanilla.fps > 0.0)
                        ImGui::TextColored(r.fps >= vanilla.fps ? ImVec4(0.4f, 0.9f, 0.5f, 1.0f)
                                                                : ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                                           "%+.0f%%", 100.0 * (r.fps / vanilla.fps - 1.0));
                    else
                        ImGui::TextDisabled(p == 1 ? "reference" : "-");
                }

                ImGui::EndTable();
            }
        }

        ImGui::SeparatorText("Edit cache (performance)");

        {
            bool cacheOn = config->DlssNrCacheEnabled.value_or_default();

            if (ImGui::Checkbox("Reuse the model's edit between runs", &cacheOn))
                config->DlssNrCacheEnabled = cacheOn;

            HelpMarker("Runs the model only one frame in N and carries its edit onto the frames between."
                       "\n\nWhat is carried is what the model CHANGED -- a per-pixel ratio -- never the"
                       "\npicture: the game's own frame is rendered fresh every frame, grass and"
                       "\ncharacters included, and the edit is moved along the game's motion vectors and"
                       "\nchecked pixel by pixel (depth, colour) before it is laid on top."
                       "\n\nThe model's broad lighting and tone (the low band) is carried everywhere and"
                       "\nborrowed from neighbours where a pixel's own history fails. Its fine detail"
                       "\n(the high band) is kept only where the checks pass, and fades with age."
                       "\n\nThe cost alternates: frames where the model runs cost what they did, the"
                       "\nothers very little. Off is Neural Rendering exactly as before."
                       "\n\nStands aside while Hold frame, Compare or a Debug view is on. D3D12 only.");

            if (cacheOn)
            {
                if (DlssNr::IsRunningVk())
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                       "D3D12 only: this game runs the Vulkan path, where it does nothing.");
                }
                else
                {
                    const auto st = DlssNr::GetCacheStatus();
                    const unsigned long long total = st.refreshes + st.cached;

                    const bool spreading = config->DlssNrCacheSpread.value_or_default() &&
                                           config->DlssNrCacheInterval.value_or_default() >= 2;
                    const unsigned int bands = std::clamp(config->DlssNrCacheInterval.value_or_default(), 2u, 4u);

                    if (total > 0 && spreading)
                        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
                                           "Model on 1 band of %u every frame - %.2f ms average per frame", bands,
                                           st.averageMs);
                    else if (total > 0)
                        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
                                           "Model on %.0f%% of frames - %.2f ms average per frame",
                                           100.0 * (double) st.refreshes / (double) total, st.averageMs);
                    else
                        ImGui::TextUnformatted("Waiting for the model to run.");

                    if (total > 0)
                        ImGui::TextDisabled("Last run: %s. Revealed since: %.1f%% (last frame %.1f%%).",
                                            st.lastRefreshReason, 100.0f * st.cumulativeRejected,
                                            100.0f * st.lastRejected);
                }

                // Starting points, not modes: each just sets the controls below, which stay editable.
                ImGui::TextUnformatted("Presets:");
                ImGui::SameLine();

                // Measured presets (Control, RTX 4070, 1440p, rendered fps; vanilla 31 fps):
                //   Max quality  model every frame, full size              31 fps, the most detail
                //   Quality      every frame, 67% + edge-aware enlargement   40 fps, as steady as vanilla
                //   Balanced     67%, every other frame, smoothed            48 fps, the least flicker
                //   Performance  67%, one in three, smoothed                 53 fps
                // All with the temporal stabiliser, which halved the worst flicker in every mode.
                auto preset = [&](unsigned int interval, float scale, float temporal)
                {
                    config->DlssNrCacheInterval = interval;
                    config->DlssNrCacheAdaptive = interval > 1;
                    config->DlssNrCacheAdaptiveThreshold = 0.10f;
                    config->DlssNrCacheHighDecay = 0.97f;
                    config->DlssNrCacheDepthTolerance = 0.10f;
                    config->DlssNrCacheColourTolerance = 0.50f;
                    config->DlssNrCacheRefreshBlend = 1.0f;
                    config->DlssNrCacheModelHistory = 1u;
                    config->DlssNrCacheBilateral = true;
                    config->DlssNrCacheCrossfade = true;
                    config->DlssNrCacheStabilize = 0.5f;
                    config->DlssNrCacheDespeckle = true;
                    config->DlssNrCacheTemporal = temporal;
                    config->DlssNrCacheLowTemporal = 0.95f;
                    config->DlssNrCacheSpread = false;
                    config->DlssNrWorkingScale = scale;
                    config->DlssNrJbuUpsample = scale < 0.999f;
                    config->DlssNrTransfer = 1u;
                };

                if (ImGui::SmallButton("Max quality"))
                    preset(1, 1.0f, 0.5f);

                ImGui::SameLine();

                if (ImGui::SmallButton("Quality"))
                    preset(1, 0.67f, 0.5f);

                ImGui::SameLine();

                if (ImGui::SmallButton("Balanced"))
                    preset(2, 0.67f, 0.5f);

                ImGui::SameLine();

                if (ImGui::SmallButton("Performance"))
                    preset(3, 0.67f, 0.6f);

                HelpMarker("Measured in Control (RTX 4070, 1440p, frames the game renders; vanilla 31 fps):"
                           "\n\n  Max quality  the model every frame, full size: 31 fps, the most fine detail"
                           "\n  Quality      every frame at 67%, edge-aware enlargement: 40 fps, as steady as"
                           "\n               vanilla, the steadiest frame times"
                           "\n  Balanced     67%, every other frame, smoothed: 48 fps, the least flicker of all"
                           "\n  Performance  67%, one frame in three: 53 fps"
                           "\n\nAll four use the temporal stabiliser. With frame generation, pick the preset that keeps"
                           "\nthe game's own frame rate above ~45 fps and keep the generation factor modest (x2-x3):"
                           "\ngenerated frames are only as clean as the real frames they are built from.");

                int interval = (int) config->DlssNrCacheInterval.value_or_default();

                if (ImGui::SliderInt("Max frames between runs", &interval, 1, 8))
                    config->DlssNrCacheInterval = (uint32_t) std::clamp(interval, 1, 16);

                HelpMarker("The model runs at least once in this many frames. 2 halves its cost, 3 thirds"
                           "\nit, and so on -- less the small cost of carrying the edit."
                           "\n\n1 runs it every frame, which only makes sense with the multi-pass gains below."
                           "\n\nWith Even frame times on, this is the number of bands instead (2 to 4).");

                bool spread = config->DlssNrCacheSpread.value_or_default();

                if (ImGui::Checkbox("Even frame times (one band per frame)", &spread))
                    config->DlssNrCacheSpread = spread;

                HelpMarker("Instead of the whole frame one frame in N, the model runs on one horizontal band"
                           "\nof EVERY frame -- the frame cut into N bands, a different one each frame. The"
                           "\nsaving is about the same, but every frame costs the same."
                           "\n\nUneven frame times -- one heavy frame, then light ones -- are what frame"
                           "\npacing, Reflex and frame generation handle worst: they are felt as input lag and"
                           "\nmicro-stutter. This removes them."
                           "\n\nEach band keeps a little of the picture above and below it for context and"
                           "\nblends into its neighbours. Uses one model per band (about one extra model's"
                           "\nworth of video memory in total). Needs 2 or more frames between runs.");

                bool adaptive = config->DlssNrCacheAdaptive.value_or_default();

                if (ImGui::Checkbox("Adapt to motion", &adaptive))
                    config->DlssNrCacheAdaptive = adaptive;

                if (adaptive)
                {
                    const auto st = DlssNr::GetCacheStatus();
                    static const char* regimeNames[] = { "still -- model half as often",
                                                         "moving -- interval as set",
                                                         "fast motion -- model twice as often" };
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", regimeNames[std::clamp(st.regime, 0, 2)]);
                }

                HelpMarker("Changes how often the model runs with how much of the picture is changing, in"
                           "\nthree steady regimes rather than runs fired early at random moments (an uneven"
                           "\nrhythm flickers by itself):"
                           "\n\n  standing still  -- half as often (up to 1 frame in 8): nothing to redo"
                           "\n  moving          -- the interval set above"
                           "\n  fast motion     -- twice as often: more of each frame is new"
                           "\n\nA regime only changes once the motion has clearly left it, so the rhythm holds.");

                if (adaptive)
                {
                    float thr = config->DlssNrCacheAdaptiveThreshold.value_or_default();

                    if (ImGui::SliderFloat("Fast-motion threshold", &thr, 0.01f, 0.5f, "%.2f", ImGuiSliderFlags_Logarithmic))
                        config->DlssNrCacheAdaptiveThreshold = std::clamp(thr, 0.001f, 1.0f);

                    HelpMarker("The share of each frame that has to be newly revealed for the fast-motion"
                               "\nregime; a twentieth of it counts as standing still. Lower switches to the fast"
                               "\nregime sooner.");
                }

                float depthTol = config->DlssNrCacheDepthTolerance.value_or_default();

                if (ImGui::SliderFloat("Depth tolerance", &depthTol, 0.01f, 0.5f, "%.2f", ImGuiSliderFlags_Logarithmic))
                    config->DlssNrCacheDepthTolerance = std::clamp(depthTol, 0.005f, 1.0f);

                HelpMarker("How far a pixel's depth may move between frames and still count as the same"
                           "\nsurface. Relative: 0.10 is ten percent. A pixel that fails takes the edit of"
                           "\nits own surface from its neighbours instead of its own history."
                           "\n\nToo low and everything close to the camera fails while you walk; too high"
                           "\nand an edit can slide off a silhouette onto what is behind it.");

                float colourTol = config->DlssNrCacheColourTolerance.value_or_default();

                if (ImGui::SliderFloat("Colour tolerance", &colourTol, 0.1f, 4.0f, "%.2f stops",
                                       ImGuiSliderFlags_Logarithmic))
                    config->DlssNrCacheColourTolerance = std::clamp(colourTol, 0.05f, 8.0f);

                HelpMarker("How much the frame under a pixel may change brightness and still keep the"
                           "\nmodel's fine detail. Swaying grass, water, particles and hair fail this and"
                           "\nkeep only the broad edit -- which is what keeps them from smearing.");

                float decay = config->DlssNrCacheHighDecay.value_or_default();

                if (ImGui::SliderFloat("Detail persistence", &decay, 0.5f, 1.0f, "%.2f"))
                    config->DlssNrCacheHighDecay = std::clamp(decay, 0.0f, 1.0f);

                HelpMarker("How much of the fine detail's confidence survives each frame without the"
                           "\nmodel. Lower fades it toward the broad edit faster, which is steadier but"
                           "\nsofter between runs -- and too low makes the detail visibly pulse at the"
                           "\nrefresh rate. Not used with Even frame times, where nothing waits long.");

                float blend = config->DlssNrCacheRefreshBlend.value_or_default();

                if (ImGui::SliderFloat("Refresh blend", &blend, 0.1f, 1.0f, "%.2f"))
                    config->DlssNrCacheRefreshBlend = std::clamp(blend, 0.05f, 1.0f);

                HelpMarker("When the model runs, how much of its new edit replaces the carried one."
                           "\n1 takes it whole. Lower softens the step you may see each time it runs, at"
                           "\nthe cost of the edit lagging a little behind the picture.");

                static const char* historyNames[] = { "The game's vectors", "Accumulated motion",
                                                      "Reset every run" };
                int history = (int) config->DlssNrCacheModelHistory.value_or_default();

                if (history < 0 || history > 2)
                    history = 1;

                if (ImGui::Combo("Model history", &history, historyNames, IM_ARRAYSIZE(historyNames)))
                    config->DlssNrCacheModelHistory = (uint32_t) history;

                HelpMarker("The model keeps its own temporal history and moves it along the motion"
                           "\nvectors it is handed. Run one frame in N, it has missed N-1 frames of motion."
                           "\n\nAccumulated motion hands it the motion since it last ran, so its history"
                           "\nlands where the scene went. The game's vectors tell it one frame's worth."
                           "\nReset discards its history every run, which is stable but noisier.");

                float stabilize = config->DlssNrCacheStabilize.value_or_default();

                if (ImGui::SliderFloat("Anti-flicker", &stabilize, 0.0f, 2.0f, stabilize <= 0.0f ? "off" : "%.2f stops"))
                    config->DlssNrCacheStabilize = std::clamp(stabilize, 0.0f, 4.0f);

                HelpMarker("Each time the model runs, a pixel that is still the same surface may change"
                           "\nbrightness by at most this much. The model re-decides small things every run --"
                           "\nthat is its detail, and it passes. Now and then it re-decides a dark patch by a"
                           "\nstop or more and back again: that is the black popping, and this holds it."
                           "\n\nLower is steadier; too low and genuine changes (a light switching on) arrive"
                           "\nover a few frames instead of at once. 0 turns it off.");

                float temporal = config->DlssNrCacheTemporal.value_or_default();

                if (ImGui::SliderFloat("Temporal stability", &temporal, 0.0f, 0.9f, temporal <= 0.0f ? "off" : "%.2f"))
                    config->DlssNrCacheTemporal = std::clamp(temporal, 0.0f, 0.9f);

                HelpMarker("Blends what is shown with last frame's, moved along the motion vectors -- but only"
                           "\nwithin the range this frame's own neighbourhood spans, as TAA does. What the model"
                           "\nre-decides from frame to frame (flicker, dark specks, detail that swims) is averaged"
                           "\nout; anything genuinely new is let through at once, so nothing trails."
                           "\n\nWith frame generation it matters twice: the generated frames are built from two real"
                           "\nones, and an edit that flickers between them becomes ghosting in every frame between."
                           "\n0 is off; 0.5 is a good default; higher is steadier and slower to change.");

                float lowTemporal = config->DlssNrCacheLowTemporal.value_or_default();

                if (ImGui::SliderFloat("Luminance stability (OLED)", &lowTemporal, 0.0f, 0.95f,
                                       lowTemporal <= 0.0f ? "off" : "%.2f"))
                    config->DlssNrCacheLowTemporal = std::clamp(lowTemporal, 0.0f, 0.95f);

                HelpMarker("Holds the brightness that trembles region by region. The model sometimes lifts and"
                           "\ndrops the light of a whole area a little from frame to frame; per pixel nothing"
                           "\nlooks wrong, so the stabiliser above lets it through, but on an OLED, black around"
                           "\nit, the area visibly breathes."
                           "\n\nThis eases the edit's regional light in time on its own. A real change of light --"
                           "\na third of a stop or more -- comes through at once. 0 is off; higher is steadier.");

                bool crossfade = config->DlssNrCacheCrossfade.value_or_default();

                if (ImGui::Checkbox("Smooth model updates", &crossfade))
                    config->DlssNrCacheCrossfade = crossfade;

                HelpMarker("Between two model runs, what is shown walks steadily toward the model's latest answer"
                           "\nand arrives exactly when the model runs again. Without it the change lands all at once"
                           "\non the frame the model runs -- the step that makes distant things blink at longer"
                           "\nintervals. Nothing is averaged: the model's answer is always reached in full.");

                bool despeckle = config->DlssNrCacheDespeckle.value_or_default();

                if (ImGui::Checkbox("Despeckle", &despeckle))
                    config->DlssNrCacheDespeckle = despeckle;

                HelpMarker("Bounds each new model edit by its eight neighbours'. The model's typical failure"
                           "\nin shadows is a speck: a few pixels it suddenly darkens while everything around"
                           "\nthem stays put, and next run they are back. A pixel darker than all of its"
                           "\nneighbours is that speck, not structure; a real edge has neighbours on its own"
                           "\nside that agree with it, so edges keep their shape.");

                bool bilateral = config->DlssNrCacheBilateral.value_or_default();

                if (ImGui::Checkbox("Borrow from the same surface", &bilateral))
                    config->DlssNrCacheBilateral = bilateral;

                HelpMarker("Where a pixel's own history was rejected, borrow the edit only from neighbours"
                           "\nat a similar depth and brightness, rather than from whatever is nearest. Keeps"
                           "\na character's edit off the wall behind them.");

                ImGui::SetNextItemOpen(true, ImGuiCond_Once);

                if (ImGui::TreeNode("Multi-pass (amplify the model's edit)"))
                {
                    float lowGain = config->DlssNrCacheLowGain.value_or_default();

                    if (ImGui::SliderFloat("Lighting gain", &lowGain, 0.0f, 3.0f, "%.2f"))
                        config->DlssNrCacheLowGain = std::clamp(lowGain, 0.0f, 4.0f);

                    float highGain = config->DlssNrCacheHighGain.value_or_default();

                    if (ImGui::SliderFloat("Detail gain", &highGain, 0.0f, 3.0f, "%.2f"))
                        config->DlssNrCacheHighGain = std::clamp(highGain, 0.0f, 4.0f);

                    if (ImGui::SmallButton("Reset##cachegains"))
                    {
                        config->DlssNrCacheLowGain = 1.0f;
                        config->DlssNrCacheHighGain = 1.0f;
                    }

                    HelpMarker("Amplifies the model's single-pass edit instead of running it twice. The"
                               "\nlighting (broad) and detail (fine) parts get separate gains, in log space,"
                               "\nso 2 doubles the edit's effect in stops rather than its brightness."
                               "\n\nThe right values come from comparing real one-pass and two-pass captures:"
                               "\ntools/dlssnr_cache/calibrate_multipass.py fits them. 1 and 1 is one pass.");

                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Character priority (stencil)"))
                {
                    bool stencil = config->DlssNrCacheStencil.value_or_default();

                    if (ImGui::Checkbox("Prioritise stencil-marked pixels", &stencil))
                        config->DlssNrCacheStencil = stencil;

                    HelpMarker("Many engines mark characters in the depth buffer's stencil plane. Pixels where"
                               "\n(stencil & mask) == value get stricter checks and count four times toward an"
                               "\nearly model run."
                               "\n\nWhich bits a game uses is not documented: pick cache debug view \"Stencil\""
                               "\nbelow, look at what a character is coloured, and narrow it down.");

                    if (stencil)
                    {
                        int mask = (int) config->DlssNrCacheStencilMask.value_or_default();

                        if (ImGui::InputInt("Mask", &mask))
                            config->DlssNrCacheStencilMask = (uint32_t) std::clamp(mask, 0, 255);

                        int ref = (int) config->DlssNrCacheStencilRef.value_or_default();

                        if (ImGui::InputInt("Value", &ref))
                            config->DlssNrCacheStencilRef = (uint32_t) std::clamp(ref, 0, 255);

                        const auto st = DlssNr::GetCacheStatus();

                        if (st.exists && !st.stencilAvailable)
                            ImGui::TextDisabled("No stencil plane found on this game's depth buffer (yet).");
                    }

                    ImGui::TreePop();
                }

                static const char* cacheDebugNames[] = { "Off", "Confidence", "Lighting band", "Detail band",
                                                         "Stencil" };
                int cacheDebug = (int) config->DlssNrCacheDebugView.value_or_default();

                if (cacheDebug < 0 || cacheDebug > 4)
                    cacheDebug = 0;

                if (ImGui::Combo("Cache debug view", &cacheDebug, cacheDebugNames, IM_ARRAYSIZE(cacheDebugNames)))
                    config->DlssNrCacheDebugView = (uint32_t) cacheDebug;

                HelpMarker("Confidence: green where a pixel keeps its own carried detail, red where it was"
                           "\nrejected and borrows its surface's broad edit. Lighting and Detail show the two"
                           "\nbands of the edit, centred on grey. Stencil colours each stencil value"
                           "\ndifferently, and magenta marks the pixels the priority settings select.");

                int dumpFrames = (int) config->DlssNrCacheDumpFrames.value_or_default();

                if (ImGui::SliderInt("Dump length", &dumpFrames, 2, 32))
                    config->DlssNrCacheDumpFrames = (uint32_t) std::clamp(dumpFrames, 2, 32);

                const auto st = DlssNr::GetCacheStatus();

                if (st.dumpActive)
                    ImGui::BeginDisabled();

                if (ImGui::Button("Dump frames for measurement"))
                    DlssNr::RequestCacheDump();

                if (st.dumpActive)
                    ImGui::EndDisabled();

                HelpMarker("Writes this many consecutive frames to dlssnr-cachedump beside OptiScaler: the"
                           "\nframe, the model's frame, motion and depth. The model runs on every one of them,"
                           "\nso each has its own ground truth. tools/dlssnr_cache/measure_reprojection.py then"
                           "\nmeasures how well a carried edit matches the real one 1, 2, 4 and 8 frames on."
                           "\n\nUses a lot of memory (about 120 MB per frame at 1440p) and stutters while it"
                           "\nwrites. Move the camera while it records -- a still frame measures nothing.");

                if (st.dumpActive)
                    ImGui::TextDisabled("Recording...");
                else if (st.dumpWritten > 0)
                    ImGui::TextDisabled("Last dump: %u frames written.", st.dumpWritten);
            }
        }

        // The other road to an even frame cost: no cache, no bands -- the model every frame, smaller.
        if (ImGui::Button("Even cost without bands: model every frame at 60%"))
        {
            config->DlssNrCacheEnabled = false;
            config->DlssNrWorkingScale = 0.6f;
            config->DlssNrJbuUpsample = true;
            config->DlssNrTransfer = 1u;
        }

        HelpMarker("Turns the edit cache off and runs the model on every frame at 60% of the frame's size"
                   "\n(about a third of the cost), brought back up guided by the full-size frame."
                   "\n\nNothing is carried between frames and there are no bands, so there is nothing to"
                   "\npop or stutter -- the model's own temporal behaviour, just cheaper. It softens the"
                   "\nfinest synthesised detail a little. Compare it with the benchmark above.");

        ImGui::SeparatorText("How much of it lands");

        float transfer = config->DlssNrTransferStrength.value_or_default();
        if (ImGui::SliderFloat("Detail strength", &transfer, 0.0f, 2.0f, "%.2f"))
            config->DlssNrTransferStrength = transfer;

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##detail"))
            config->DlssNrTransferStrength = 1.0f;

        HelpMarker("How far the frame moves toward the model's picture."
                       "\n\nThe model's answer is not added to the frame -- it is a complete picture of its"
                       "\nown, rescaled so its luminance sits where the original says it should. This"
                       "\nblends between the two, so both ends are real pictures and everything between"
                       "\nthem is one too."
                       "\n\n0 gives back exactly what the upscaler produced. 1 is the model's picture."
                       "\n\nAbove 1 carries on past it in the same direction, which is not something the"
                       "\nmodel asked for -- use it to see what it is doing, then come back down. This"
                       "\nis the control to push if you want more effect: Intensity belongs to the model"
                       "\nand it decides what to do with it.");

        float colour = config->DlssNrColourStrength.value_or_default();
        if (ImGui::SliderFloat("Colour strength", &colour, 0.0f, 4.0f, "%.2f"))
            config->DlssNrColourStrength = colour;

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##colour"))
            config->DlssNrColourStrength = 1.0f;

        HelpMarker("Whether the model's colour arrives with its light."
                       "\n\n0 keeps the game's own hue exactly -- every pixel is the original colour with"
                       "\nonly its brightness carrying the model's verdict. Game-accurate colour, with"
                       "\nthe detail. 1 brings the model's colour as well, in its own hue, clamped into"
                       "\nAP1 so nothing unreachable is asked for."
                       "\n\nThis cannot shift hue on its own: it interpolates between two finished"
                       "\npictures rather than adding a colour difference to one, which is what used to"
                       "\nlet a warm subject come back green."
                       "\n\nAbove 1 it OVER-SATURATES: the colour keeps its hue but grows more vivid,"
                       "\nand rolls off at the edge of what the display can show rather than clipping"
                       "\ninto a flat blown patch. 1 is the model's own colour; push past it for punch.");

        // Experimental. 0 off (soft knee), 1 Neutwo + our composition, 2 Neutwo + pure-inverse replace,
        // 3 hybrid+composed, 4 hybrid+replace (identity midtones + unclipped highlights). Always shown.
        static const char* reversibleNames[] = { "Off (soft knee)", "Neutwo proxy + composed",
                                                 "Neutwo proxy + replace", "Hybrid proxy + composed",
                                                 "Hybrid proxy + replace" };
        int reversible = (int) config->DlssNrReversibleMode.value_or_default();
        if (reversible < 0 || reversible > 4)
            reversible = 0;
        if (ImGui::Combo("Reversible proxy (experimental)", &reversible, reversibleNames,
                         IM_ARRAYSIZE(reversibleNames)))
            config->DlssNrReversibleMode = (uint32_t) reversible;

        HelpMarker("What the model is shown, and how its answer comes back."
                       "\n\nOff (soft knee): the default. It rolls highlights off so hard the model"
                       "\ncannot resolve detail in them -- fine in soft-lit scenes, weak in bright ones."
                       "\n\nNeutwo composed: an unclipped curve so the model sees highlight detail, then"
                       "\neverything above (Detail/Colour strength, highlight guard, palette). It wins in"
                       "\nbright scenes, but the curve compresses MIDTONES too, so in soft-lit content it"
                       "\ncan be worse than Off. It also shifts paper white -- re-check it when you switch."
                       "\n\nHybrid composed: the best of both, and the one to use. Identity in the"
                       "\nmidtones -- as good as Off there -- and the unclipped roll only in the"
                       "\nhighlights, so it recovers the detail Off crushes without giving up the"
                       "\nmidtones Neutwo does. It barely shifts paper white."
                       "\n\nReplace: the raw model straight back through the exact inverse, none of the"
                       "\ncomposition -- no guard, no palette, no strengths. Gorgeous where there are no"
                       "\nbright lights, but they FLASH in motion. A reference, not a daily setting."
                       "\n\nHybrid replace: the raw model like Replace, but on the hybrid curve -- the"
                       "\ndecode is identity in the midtones, so the flashing is confined to genuine"
                       "\nbright highlights instead of everywhere. Most of Replace's detail, far more"
                       "\nstable. If you love the Replace look but the flicker bothers you, use this."
                       "\n\nOff is byte-identical to before.");

        ImGui::SeparatorText("Model");

        ImGui::TextUnformatted("Read when the model is built, so a change rebuilds it after a moment.");

        static const char* nrPresetNames[] = { "Default", "Preset 1", "Preset 2", "Preset 3" };
        int preset = (int) config->DlssNrPreset.value_or_default();
        if (ImGui::Combo("Model preset", &preset, nrPresetNames, IM_ARRAYSIZE(nrPresetNames)))
            config->DlssNrPreset = (uint32_t) preset;

        HelpMarker("Default leaves the choice to the model."
                       "\n\nNot the same scale as the super resolution or ray reconstruction presets --"
                       "\nthe same number means something different here.");

        static const char* nrStyleNames[] = { "Default (standard)", "Natural", "Cinematic" };
        int style = (int) config->DlssNrStyle.value_or_default();

        if (style > 2)
            style = 2;

        if (ImGui::Combo("Style", &style, nrStyleNames, IM_ARRAYSIZE(nrStyleNames)))
            config->DlssNrStyle = (uint32_t) style;

        HelpMarker("The model's own processing profiles."
                   "\n\nDefault (standard): the strongest. Boosts local contrast and deepens"
                   "\nlighting, and can oversaturate or look stylised -- most of what reads as"
                   "\n'the model changed my game's look' is this profile."
                   "\n\nNatural: the same detail work with a gentler hand. Keeps skin tones and"
                   "\ntonal balance closer to what the game rendered."
                   "\n\nCinematic: tones down the shine and over-processing for a film-like look."
                   "\n\nRead when the model is built, so a change rebuilds it after a moment. The"
                   "\nnames come from community testing; NVIDIA ships no names in the binaries.");

        DeferredSlider("Intensity", &config->DlssNrIntensity, 0.0f, 2.0f, 1.0f);

        HelpMarker("The model's own strength control, applied inside it. Distinct from detail"
                       "\nstrength above, which scales the result afterwards.");

        DeferredSlider("Local structure", &config->DlssNrLocalStructure, 0.0f, 2.0f, 1.0f);

        DeferredSlider("Local tone", &config->DlssNrLocalTone, 0.0f, 2.0f, 1.0f);


        DeferredSlider("Skin structure", &config->DlssNrSkinStructure, -1.0f, 2.0f, -1.0f);

        HelpMarker("-1 means follow local structure, and is the model's own default -- it is not a"
                       "\nstrength of zero. 0 and above set skin independently of the rest of the frame.");

        bool autoMask = config->DlssNrAutoMask.value_or_default();
        if (ImGui::Checkbox("Auto skin mask", &autoMask))
            config->DlssNrAutoMask = autoMask;

        HelpMarker("Lets the model find skin itself rather than treating the frame uniformly.");

        ImGui::SeparatorText("Colour");

        ImGui::TextDisabled("The model was trained on finished, sRGB-encoded frames. The upscaler's\n"
                            "output is not one: it is linear and open-ended. These decide how it is\n"
                            "mapped into something the model recognises. A frame the game reports as\n"
                            "already tone-mapped is passed over untouched and none of this applies.");

        {
        // Logarithmic, because the useful range is not linear. A quarter to 240: the low end because
        // a frame the game already tone mapped wants roughly 1, the high end because there is no
        // principled ceiling -- this is a divisor on an open-ended linear buffer, and how far up a
        // given game needs to go is a property of that game's exposure, not of anything we can bound.
        // One tester was still improving at 100. A linear slider over that span would spend nine
        // tenths of its travel on values nobody needs and never reach the ones they do.
        // One dropdown, because there is one answer.
        //
        // This was two checkboxes that could both be on, and every attempt to stop that was a patch
        // on a shape that should not have existed. Greying deadlocked -- each disabled the other, so
        // once both were set the only way out was a button the notice never mentioned. Clearing
        // worked but silently undid a setting somebody had made. Both were ways to stop an illegal
        // state being REACHED; a single choice cannot reach it, because there is only one value to
        // be in.
        //
        // Each option also says whether it can actually do anything in THIS game, in colour, so the
        // choice is made on what is available rather than on what sounds best.
        {
            const auto ex = DlssNr::GameExposureStatus();
            const bool vk = DlssNr::IsRunningVk();
            const bool haveExposure = vk ? DlssNr::ExposureOfferedVk() : ex.everOffered;

            const float anchorNow = DlssNr::ExposureScan::BestValue();
            const bool haveAnchor = !DlssNr::ExposureScan::Anchors().empty();

            static const char* sourceNames[] = { "Paper white only", "The game's own exposure",
                                                 "A buffer the scan found", "Automatic (measured from the frame)" };

            int source = (int) config->DlssNrWhitePointSource.value_or_default();

            if (source < 0 || source > 3)
                source = 0;

            if (ImGui::Combo("White point from", &source, sourceNames, IM_ARRAYSIZE(sourceNames)))
            {
                config->DlssNrWhitePointSource = (uint32_t) source;

                // Nothing else to set. The scan asks the source whether it is wanted, so choosing
                // it here is the whole of switching it on -- there is no second flag to keep in
                // step, and so no way for the two to disagree.
            }

            HelpMarker("Where the number that divides the frame comes from."
                           "\n\nPaper white only -- the slider below and nothing else. Right for a"
                           "\ngame whose exposure never moves, wrong the moment it does: one"
                           "\nconstant cannot serve a cave and a field."
                           "\n\nThe game's own exposure -- read from the texture the game hands"
                           "\nthe upscaler. The best source there is, because it is decided"
                           "\nupstream and nothing this pass does can move it. Not every game"
                           "\nsupplies one."
                           "\n\nA buffer the scan found -- for games that compute an exposure and"
                           "\nnever pass it on. A GUESS: candidates are matched by shape, and in"
                           "\nGTA V the best one tracks the real exposure but at its own scale,"
                           "\nwhich the anchor's ratio cancels. Needs anchoring once, and checking"
                           "\nafterwards.");

            // Availability, in colour, for the option currently chosen.
            if (source == 3)
            {
                const float wp = DlssNr::AutoWhitePoint();

                if (wp > 0.0f)
                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "Scene average  ->  white point %.2f (adapting toward %.2f)", wp,
                                       DlssNr::AutoWhiteMeasured());
                else
                    ImGui::TextDisabled("Measuring the scene...");
            }
            else if (source == 1)
            {
                if (!vk && ex.seenFrames == 0)
                    ImGui::TextDisabled("Waiting for a frame...");
                else if (!haveExposure)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                       "This game supplies no exposure -- paper white is in use. Try "
                                       "the scan instead.");
                else if (!vk && ex.unreliable)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                       "This game's \"exposure\" jumps by thousands of times, so it is not "
                                       "one -- ignored, paper white is in use.");
                else if (vk)
                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "This game supplies an exposure and it is being read.");
                else if (ex.exposure > 1e-6f)
                {
                    const float trim =
                        std::clamp(config->DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "Game exposure %.4f  ->  white point %.2f%s", ex.exposure,
                                       ex.preExposure / ex.exposure * trim,
                                       ex.offeredNow ? "" : "  (held: absent this frame)");
                }
                else
                    ImGui::TextDisabled("Reading the exposure...");
            }
            else if (source == 2)
            {
                // "Nothing found" and "found several, none of them moving" are different states,
                // and this said the first for both. In GTA V the log carried eight candidates while
                // the panel claimed there were none, which reads as the scan being broken when what
                // it actually needs is for the light to change.
                if (anchorNow <= 0.0f)
                {
                    const unsigned int watching = (unsigned int) DlssNr::ExposureScan::Report().size();

                    if (watching == 0)
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           "Nothing in this game is shaped like an exposure.");
                    else
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           "Watching %u, none moving yet -- go between light and shade.",
                                           watching);
                }
                else if (!haveAnchor)
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                       "Found one. Set paper white below until the picture looks "
                                       "right, then press Anchor here.");
                // Once anchored, the scan -> white point readout sits above the sliders below; it is
                // not repeated up here.
            }
            else if (haveExposure)
            {
                ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                   "This game supplies an exposure -- the option above would use it.");
            }
        }






        // A measured suggestion for paper white used to sit here and has been withdrawn.
        //
        // It took the 90th percentile of per-tile peak luminance from the untouched frame, which is a
        // statement about scene content rather than about the buffer's scale. In Nioh 3, where the
        // right answer is about 240, it offered 8 -- because most tiles are shadow and the percentile
        // sits wherever most tiles are. The guard meant to catch that compared each tile against the
        // frame's own brightest, which is scale-free and therefore passes on a black screen: the same
        // relative-threshold mistake the white point meter was removed for, made a second time.
        //
        // A wrong number offered confidently is worse than no number, so nothing is offered. What
        // replaces it has to be a measurement of the game's own exposure rather than of its scenery:
        // the exposure texture where a game supplies one, and otherwise the ratio between the
        // scene-referred buffer and the finished frame, which is that exposure by definition.

        // Two controls, not one control with two meanings.
        //
        // These are different quantities. The manual path wants an absolute divisor on an open-ended
        // linear buffer -- Nioh 3 needs about 240 -- and the exposure path wants a multiplier on a
        // number the game already supplied, where 1 is correct and anything far from it says the read
        // is wrong rather than that somebody prefers it.
        //
        // They used to share one stored value, narrowed to 0.25..4 when the toggle was on. That kept
        // a ruinous value unreachable but left two worse problems: moving the slider in one mode
        // silently destroyed the number found in the other, and there was no way back to "just take
        // the game's answer" short of knowing that the number for it was 1. Separate values fix both.
        // Switching modes is now non-destructive in both directions.
        // The trim belongs to both automatic sources, since both end in "the game's number times a
        // little". Only the manual source gets the absolute slider.
        // One slider per source, each remembering its own number.
        //
        // A trim on the game's exposure and a trim on a buffer the scan found are trims on different
        // things, and a value found against one means nothing against the other. Sharing them meant
        // changing source silently carried a number across, so a picture that had been tuned came
        // back wrong for a reason nothing on screen explained.
        //
        // The scan before it is anchored is the exception, and it has to be: anchoring captures an
        // absolute white point, so there must be an absolute slider to set. Showing a trim there
        // asked people to "set paper white below" next to a control that was not paper white.
        const int wpSource = (int) config->DlssNrWhitePointSource.value_or_default();

        // Which anchor row the paper-white slider edits, or -1 for the live unanchored point. Menu-
        // local and not persisted; the anchor block below sets it when a row is clicked. Declared
        // here because both the slider (this block) and the table (below) read it in the same frame.
        static int selectedAnchor = -1;
        auto anchors = DlssNr::ExposureScan::Anchors();
        if (selectedAnchor >= (int) anchors.size())
            selectedAnchor = -1;

        if (wpSource == 2)
        {
            const bool editingRow = selectedAnchor >= 0 && selectedAnchor < (int) anchors.size();

            // The single scan -> white point readout, above the sliders it explains.
            if (!anchors.empty())
            {
                const float liveScan = DlssNr::ExposureScan::BestValue();

                if (liveScan > 0.0f)
                {
                    const float w = DlssNr::ExposureScan::AnchoredWhitePoint(
                        liveScan, config->DlssNrScanInverted.value_or_default(),
                        config->DlssNrScanTrim.value_or_default());

                    ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                       "Scan %.5f  ->  white point %.2f   (%u point%s)", liveScan, w,
                                       (unsigned) anchors.size(), anchors.size() == 1 ? "" : "s");
                }
            }

            // Paper white shows only when there is a point to set: before the first anchor, or when a
            // row is selected to edit. Once points exist and none is selected, the white point is fixed
            // by the anchors and only the trim adjusts the live picture -- so the trim takes the
            // slider's place, the same shape as the game-exposure source.
            const bool showPaperWhite = anchors.empty() || editingRow;

            if (showPaperWhite)
            {
                float pw = editingRow ? anchors[selectedAnchor].white
                                      : config->DlssNrWhitePointScale.value_or_default();

                char lbl[48];
                if (editingRow)
                    snprintf(lbl, sizeof(lbl), "Paper white (editing point %d)", selectedAnchor + 1);
                else
                    snprintf(lbl, sizeof(lbl), "Paper white");

                if (ImGui::SliderFloat(lbl, &pw, 0.25f, 2000.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
                {
                    if (editingRow)
                    {
                        DlssNr::ExposureScan::AnchorSetWhite(selectedAnchor, pw);
                        config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                    }
                    else
                        config->DlssNrWhitePointScale = pw;
                }

                HelpMarker("The white point for the selected calibration point, or -- with no row"
                               "\nselected -- the value the next Anchor press captures."
                               "\n\nSet it until the picture looks right here, then Anchor. Move to very"
                               "\ndifferent light and do it again: two points fix the buffer's real"
                               "\nrelationship and the white point holds between them. Click a row below"
                               "\nto come back and adjust that point; click it again to let go.");
            }

            // The trim multiplies the interpolated result, and in the steady state it is the control
            // that stands in for paper white: adjust it until the picture looks right in the current
            // light, then Anchor bakes that trimmed value into a new point and resets the trim to 1.
            if (!anchors.empty())
            {
                float trim = config->DlssNrScanTrim.value_or_default();

                if (ImGui::SliderFloat("Trim (x the scan)", &trim, 0.25f, 4.0f, "%.2fx",
                                       ImGuiSliderFlags_Logarithmic))
                    config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);

                ImGui::SameLine();

                if (ImGui::SmallButton("Reset##scantrim"))
                    config->DlssNrScanTrim = 1.0f;

                HelpMarker("A multiplier on the scan's white point, and the control you adjust between"
                               "\nanchor points: dial it until the picture looks right in the current"
                               "\nlight, then press Anchor here -- it captures the trimmed value as a new"
                               "\npoint and resets the trim to 1.");
            }
        }
        else if (wpSource == 3)
        {
            float trim = config->DlssNrWhitePointTrim.value_or_default();

            if (ImGui::SliderFloat("Trim (x the measured scene)", &trim, 0.25f, 4.0f, "%.2fx",
                                   ImGuiSliderFlags_Logarithmic))
                config->DlssNrWhitePointTrim = std::clamp(trim, 0.25f, 4.0f);

            ImGui::SameLine();

            if (ImGui::SmallButton("Reset##autotrim"))
                config->DlssNrWhitePointTrim = 1.0f;

            float local = config->DlssNrAutoLocal.value_or_default();

            if (ImGui::SliderFloat("Local adaptation", &local, 0.0f, 1.0f, "%.2f"))
                config->DlssNrAutoLocal = std::clamp(local, 0.0f, 1.0f);

            if (local > 0.0f)
            {
                float shadows = config->DlssNrAutoLocalShadows.value_or_default();

                if (ImGui::SliderFloat("...in shadows too", &shadows, 0.0f, 1.0f, "%.2f"))
                    config->DlssNrAutoLocalShadows = std::clamp(shadows, 0.0f, 1.0f);

                HelpMarker("Whether dark regions are brightened for the model as well. 0 (the default) leaves"
                           "\nthem as dark as the scene has them: measured, brightening them shows the model the"
                           "\nnoise in its shadows and it answers with specks that pop. Raise it only if a game's"
                           "\ndark areas get no detail at all.");
            }

            HelpMarker("How much each region of the frame gets its own white point, from its own brightness."
                       "\n\nOne number for a whole frame cannot serve a lit window and the shadow beside it: high"
                       "\nenough for the window, it shows the model the shadow as black -- and the model answers"
                       "\nblack with specks that pop -- low enough for the shadow, it flattens the window. With"
                       "\nthis, every region is shown to the model properly exposed."
                       "\n\nThe frame itself keeps its contrast: the model's answer comes back as a ratio, divided"
                       "\nby the same local value it was shown with. 0 is one white point for the whole frame.");

            HelpMarker("The white point follows the scene: its average brightness, measured on the frame"
                       "\nbefore Neural Rendering touches it, sets where white is, and eases over about"
                       "\nhalf a second like an eye adapting. Bright exteriors and dark interiors each get"
                       "\nthe value that suits them, which no single paper white can -- high enough for"
                       "\ndaylight, a fixed one shows the model a black picture indoors, and the model"
                       "\nanswers that with speckle that flickers."
                       "\n\nThis multiplies the measured value: above 1 the model sees a darker picture"
                       "\n(highlights keep more detail), below 1 a brighter one.");
        }
        else if (wpSource == 1)
        {
            const bool ofScan = false;

            float trim = ofScan ? config->DlssNrScanTrim.value_or_default()
                                : config->DlssNrWhitePointTrim.value_or_default();

            if (ImGui::SliderFloat(ofScan ? "Trim (x the scan)" : "Trim (x the game's exposure)", &trim,
                                   0.25f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
            {
                if (ofScan)
                    config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);
                else
                    config->DlssNrWhitePointTrim = std::clamp(trim, 0.25f, 4.0f);
            }

            ImGui::SameLine();

            // Deliberately always present rather than greyed at 1. The point of it is that the safe
            // value is one click away without having to know what the safe value is.
            if (ImGui::SmallButton("Reset##wptrim"))
            {
                if (ofScan)
                    config->DlssNrScanTrim = 1.0f;
                else
                    config->DlssNrWhitePointTrim = 1.0f;
            }

            HelpMarker("A multiplier on the exposure the game supplied. 1.00x takes its number"
                           "\nexactly, and that is the right answer here."
                           "\n\nThis is not a fudge factor. If a game needs the trim far from 1 to look"
                           "\nright, that is evidence the exposure being read is wrong for that game,"
                           "\nnot that the game wants trimming. Somewhere around 0.8 to 1.25 is honest"
                           "\ntuning; reaching for 4 means something upstream is broken and the trim is"
                           "\nhiding it."
                           "\n\nYour manual paper white is kept separately and comes back untouched if"
                           "\nyou switch the option above off.");
        }
        else
        {
            // Logarithmic, because the useful range is not linear. A quarter to 2000: the low end
            // because a frame the game already tone mapped wants roughly 1, the high end because
            // there is no principled ceiling -- this is a divisor on an open-ended linear buffer, and
            // how far up a given game needs to go is a property of that game's exposure rather than
            // of anything that can be bounded here. One tester was still improving at 100.
            float wpScale = config->DlssNrWhitePointScale.value_or_default();

            if (ImGui::SliderFloat("Paper white", &wpScale, 0.25f, 2000.0f, "%.2fx",
                                   ImGuiSliderFlags_Logarithmic))
                config->DlssNrWhitePointScale = wpScale;

        HelpMarker("What the frame is divided by before the model sees it. There is no other white"
                       "\npoint; this is the whole of it."
                       "\n\nThe model was trained on finished frames where white sits at 1. The"
                       "\nupscaler's output is linear and open-ended, so something has to say where"
                       "\nwhite is -- and where the game's DLSS buffer is linear HDR, that number is"
                       "\nrarely anywhere near 1. Measured in Monster Hunter Wilds it takes 16 or more"
                       "\nbefore the model's detail reaches the frame at all, and the value that suits"
                       "\na shaded camp is still too small for the same game out in daylight."
                       "\n\nToo low and almost every pixel trips the soft knee: the model is shown a"
                       "\nflat near-white picture, its answer is scaled away, and only its hue"
                       "\nsurvives -- which reads as a colour cast rather than as lost detail. Too"
                       "\nhigh and it is shown an underexposed one, its answer degrades, and this same"
                       "\nnumber multiplies that error on the way out."
                       "\n\nRaise it until the picture stops improving. Past that point it does not"
                       "\nplateau, it gets worse in the other direction."
                       "\n\nThis was once a multiplier on a measured white point. The measurement is"
                       "\ngone: it read scene brightness rather than where white belongs, handed the"
                       "\nmodel a picture three times too dark, and left the highlight path nothing to"
                       "\ngive back."
                       "\n\nAt strength zero the frame is still bit-identical whatever this says.");
        }

        // Highlight guard, directly under the white point / trim -- it bounds the model's edit and
        // belongs with the exposure controls it works alongside.
        float maxRatio = config->DlssNrMaxRatio.value_or_default();
        if (ImGui::SliderFloat("Highlight guard", &maxRatio, 1.0f, 8.0f, "%.1fx"))
            config->DlssNrMaxRatio = maxRatio;

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##guard"))
            config->DlssNrMaxRatio = 2.0f;

        HelpMarker("The most the pass may move any pixel, as a multiple of what it already was, in"
                       "\nboth directions -- a pixel may not be brightened past this nor darkened past"
                       "\nits reciprocal. Lights are where the model has least to say and rescaling its"
                       "\nanswer does the most damage; 2x leaves detail intact while stopping a strip"
                       "\nlight turning into a string of coloured cells. Raise it only if bright areas"
                       "\nlook clipped.");

        // Directly under the white point, because that is the number it moves and the number the
        // anchor captures. It used to sit under Inspect, a whole section away from the slider it
        // reads, which left "Anchor here" looking like a control for something else entirely.
        {
            // No checkbox here any more.
            //
            // The dropdown above says whether the scan is the white point's source, and that is
            // the only reason anybody using this would want it running. A second control could
            // only agree with the dropdown or contradict it, and both were on offer: it began as
            // a redundant question and became a way to switch off the thing the chosen source
            // depended on.
            //
            // The ini key survives as a developer override for the one case a user has no reason
            // to want -- running the scan in a game that supplies a REAL exposure, so the log can
            // compare the two. That is validation, and validation does not need a widget.
            //
            // Worth keeping written down, since the panel no longer says it: the scan matches
            // buffers by SHAPE, and shape is a weak filter. In GTA V -- a game that supplies a
            // real exposure, so the right answer sat visible beside it -- the best candidate was
            // a 1x1 R32_FLOAT that climbed in a straight line for seventeen minutes while the
            // true exposure held still. Their ratio moved 14x. That is an accumulator, not an
            // eye adaptation.

                // Only where it means something. The lamp reads the scan, so offering it beside a
                // white point that comes from the game's own exposure is offering a control that
                // cannot light up.
                bool meter = config->DlssNrScanMeter.value_or_default();

                if (config->DlssNrWhitePointSource.value_or_default() == 2 &&
                    ImGui::Checkbox("Show the light meter on screen", &meter))
                    config->DlssNrScanMeter = meter;

                HelpMarker("A lamp in the corner: red for dark, green for full light, and the"
                               "\nshades between, with the reading beside it."
                               "\n\nIt is how you see at a glance that the scan is TRACKING rather"
                               "\nthan merely running. Walk into shade and it should slide toward"
                               "\nred; step out and it should go green. If it moves the wrong way,"
                               "\nthat is what the setting above is for."
                               "\n\nPurely a readout. It changes nothing.");

            // Shown when the scan is actually running, whichever way it got switched on.
            if (DlssNr::ExposureScan::Scanning())
            {
                // Anchoring: one press, then it never needs touching again.
                //
                // The absolute white point cannot come out of a buffer whose units are unknown.
                // Every value AFTER the first can: only the ratio against the anchor is used, so
                // whatever the number means, it cancels. That is why this is a button and not a
                // measurement -- the one thing a person can supply that no amount of cleverness
                // can is "this looks right to me".
                int which = 0;
                float low = 0.0f, high = 0.0f;
                const float live = DlssNr::ExposureScan::BestValue(&which, &low, &high);

                const bool isSource = config->DlssNrWhitePointSource.value_or_default() == 2;

                // Anchor captures (currentScan, currentPaperWhite) and ADDS a row -- it does not
                // replace. One row is the old single-anchor ratio law; add a second in different
                // light and the white point is interpolated between the points, so it holds across
                // the whole range instead of only near one anchor. Greyed unless the scan is the
                // chosen source and it currently has a value to capture.
                ImGui::BeginDisabled(live <= 0.0f || !isSource);

                if (ImGui::Button("Anchor here"))
                {
                    // What to capture. Before the first point, the paper white above (an absolute value
                    // with the wide range a fresh game needs). After that, the EFFECTIVE white point the
                    // picture is showing right now -- the interpolated value times the Trim the user just
                    // dialed in -- so a second point in different light captures the trimmed look, not a
                    // frozen paper white (which would make two equal whites and a flat, non-tracking
                    // curve). The trim is reset afterwards: the new point, which the picture now passes
                    // through exactly, must not be multiplied by it a second time.
                    const float captureWhite =
                        anchors.empty()
                            ? std::max(0.01f, config->DlssNrWhitePointScale.value_or_default())
                            : std::max(0.01f, DlssNr::ExposureScan::AnchoredWhitePoint(
                                                  live, config->DlssNrScanInverted.value_or_default(),
                                                  config->DlssNrScanTrim.value_or_default()));

                    if (DlssNr::ExposureScan::AnchorAdd(live, captureWhite))
                    {
                        config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                        config->DlssNrScanTrim = 1.0f;
                        selectedAnchor = -1;
                    }
                }

                ImGui::EndDisabled();

                HelpMarker("Make the picture look right, then press this -- it captures the current look"
                               "\nas a point. For the first point use the Paper white slider above; for"
                               "\nevery point after, move to different light and use the Trim, which the"
                               "\nAnchor then bakes into a new point."
                               "\n\nThe first press calibrates one point -- the white point then"
                               "\nfollows the scan by ratio from there, as before. Walk into very"
                               "\ndifferent light, set paper white again, and press it again: the"
                               "\nsecond point pins down the buffer's real curve and everything"
                               "\nbetween the two is right, not just near one anchor. Up to eight."
                               "\n\nThe table is per game and shareable: one person calibrates a game"
                               "\nand the numbers are the same for everyone who takes the profile.");

                if (!isSource)
                    ImGui::TextDisabled("(the scan is only watching -- the white point above comes "
                                        "from somewhere else)");

                if (!anchors.empty())
                {
                    // The row nearest the live scan value (in log space) is the one driving the
                    // picture right now; mark it so the user can see which calibration is in effect.
                    int active = 0;
                    float bestDist = 1e30f;
                    const float liveLog = std::log(std::max(live, 1e-6f));

                    for (size_t i = 0; i < anchors.size(); ++i)
                    {
                        const float d =
                            std::fabs(std::log(std::max(anchors[i].scan, 1e-6f)) - liveLog);
                        if (d < bestDist)
                        {
                            bestDist = d;
                            active = (int) i;
                        }
                    }

                    for (size_t i = 0; i < anchors.size(); ++i)
                    {
                        ImGui::PushID((int) i);

                        // Delete first, so its click is never swallowed by the row-wide Selectable.
                        if (ImGui::SmallButton("x"))
                        {
                            DlssNr::ExposureScan::AnchorRemove((int) i);
                            config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                            if (selectedAnchor == (int) i)
                                selectedAnchor = -1;
                            else if (selectedAnchor > (int) i)
                                --selectedAnchor;
                            ImGui::PopID();
                            continue;
                        }

                        ImGui::SameLine();

                        const bool sel = (int) i == selectedAnchor;
                        char row[96];
                        snprintf(row, sizeof(row), "%s scan %.4f  ->  white %.2f%s",
                                 ((int) i == active && isSource) ? ">" : "  ", anchors[i].scan,
                                 anchors[i].white, sel ? "   [editing]" : "");

                        // Click selects the row (slider edits it); click again deselects (slider
                        // returns to the live unanchored point).
                        if (ImGui::Selectable(row, sel))
                            selectedAnchor = sel ? -1 : (int) i;

                        ImGui::PopID();
                    }

                    ImGui::TextDisabled("Click a row to edit it with the slider above; click it again"
                                        " to control the live point. > is the point in use now.");
                }

                // The direction flag only means anything with a single point; with two or more the
                // direction the white point moves is already fixed by the data.
                if (anchors.size() == 1)
                {
                    bool inverted = config->DlssNrScanInverted.value_or_default();
                    if (ImGui::Checkbox("The number runs the other way", &inverted))
                        config->DlssNrScanInverted = inverted;

                    HelpMarker("Flip this if the picture gets worse in the direction it should be"
                                   "\ngetting better. Most engines store an exposure that falls as"
                                   "\nthe scene brightens; some store its reciprocal, and a buffer"
                                   "\nfound by shape does not say which. Add a second anchor point in"
                                   "\ndifferent light and this is decided for you, so it disappears.");
                }

                // The scan -> white point readout is shown above the sliders now, not here.

                // Everything below is read-out rather than control: what the scan is looking at and
                // how to tell whether it found the right thing. Folded away because the two decisions
                // that matter -- anchor, and which way the number runs -- are above it.
                if (ImGui::TreeNode("Advanced"))
                {

                    const auto found = DlssNr::ExposureScan::Report();
                    const char* why = DlssNr::ExposureScan::Status();

                    if (found.empty())
                    {
                        ImGui::TextDisabled("%s", why != nullptr && why[0] != 0
                                                      ? why
                                                      : "nothing matched yet.");
                    }
                    else
                    {
                        for (size_t i = 0; i < found.size(); ++i)
                        {
                            const auto& c = found[i];

                            if (c.reads == 0)
                            {
                                ImGui::TextDisabled("%zu. %s -- not read yet", i + 1, c.shape.c_str());
                                continue;
                            }

                            // Moving is the whole signal, so it is the thing that is coloured.
                            ImGui::TextColored(c.moves ? ImVec4(0.45f, 0.8f, 0.45f, 1.0f)
                                                       : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                                               "%zu. %s = %.5f  (seen %.5f..%.5f) %s", i + 1,
                                               c.shape.c_str(), c.latest, c.lowest, c.highest,
                                               c.moves ? "MOVES" : "flat so far");
                        }

                        ImGui::TextDisabled("Walk from shade into daylight. A real exposure moves.");
                        ImGui::TextDisabled("One that only ever climbs is a counter, not an exposure.");
                    }

                    ImGui::TreePop();
                }
            }
        }


        }

        ImGui::SeparatorText("Compare");

        // Freeze the frame the model works on, so a setting change re-renders it in place -- the only
        // clean way to A/B our own settings (a moving scene confounds every other comparison). See
        // design/frame-hold.md.
        bool held = config->DlssNrHoldFrame.value_or_default();
        if (ImGui::Checkbox("Hold frame", &held))
            config->DlssNrHoldFrame = held;

        HelpMarker("Freezes the frame the model works on. While held, change paper white, the"
                       "\nstrengths, the reversible mode, the model preset -- anything below the"
                       "\nupscaler -- and only that setting moves; the scene does not."
                       "\n\nWhat it CANNOT show: DLSS/FSR/XeSS upscaler presets or anything upstream"
                       "\n(the upscaler is not re-run on a held frame), and the game's own HUD and"
                       "\npost-processing, which run after this pass and keep updating. The white"
                       "\npoint stops being measured and holds its value while frozen, so it cannot"
                       "\ndrift and confound the comparison."
                       "\n\nHide the menu and it stays held. Untoggle to resume.");

        static const char* compareNames[] = { "Off", "Side by side", "Wipe" };
        int compare = (int) config->DlssNrCompare.value_or_default();
        if (ImGui::Combo("Compare", &compare, compareNames, IM_ARRAYSIZE(compareNames)))
            config->DlssNrCompare = (uint32_t) compare;

        HelpMarker("Shows the pass against itself, so the two can be seen at once rather than"
                       "\ntoggled and remembered."
                       "\n\nSide by side puts the whole frame in each half, untouched on the left and"
                       "\nedited on the right. Both halves are squeezed horizontally to fit, so it is"
                       "\nfor looking at rather than playing in."
                       "\n\nWipe cuts a single frame at the split and resamples nothing, so the picture"
                       "\nis the right shape and can be played normally. Drag the split below; it is a"
                       "\nstored setting and stays put once the menu is closed."
                       "\n\nNeither needs the menu open to keep working. A hairline marks the join.");

        if (compare != 0)
        {
            bool swap = config->DlssNrCompareSwap.value_or_default();
            if (ImGui::Checkbox("Swap sides", &swap))
                config->DlssNrCompareSwap = swap;

            bool tags = config->DlssNrCompareTags.value_or_default();
            if (ImGui::Checkbox("Label the sides", &tags))
                config->DlssNrCompareTags = tags;

            HelpMarker("Writes which side is which onto the frame itself, so a screenshot still"
                           "\nsays so after it has left this machine. Drawn into the picture's own"
                           "\nplane: in the wipe the split reveals and hides the label exactly as it"
                           "\ndoes the images, and there is nothing to drag. Swap sides moves the"
                           "\nlabels with their pictures.");

            if (tags)
            {
                float tagScale = config->DlssNrTagScale.value_or_default();
                if (ImGui::SliderFloat("Label size", &tagScale, 0.5f, 5.0f, "%.1fx"))
                    config->DlssNrTagScale = std::clamp(tagScale, 0.5f, 5.0f);
            }

            HelpMarker("Puts the edited frame on the other side."
                           "\n\nWorth doing once you have decided which you prefer: the eye is not"
                           "\neven-handed about left and right, and a difference can read as an"
                           "\nimprovement purely from where it sits. If the same side still wins after"
                           "\nswapping, it is the pass you are seeing and not the placement.");
        }

        if (compare == 1)
        {
            float zoom = config->DlssNrCompareZoom.value_or_default();
            if (ImGui::SliderFloat("Zoom", &zoom, 1.0f, 2.0f, "%.2f"))
                config->DlssNrCompareZoom = std::clamp(zoom, 1.0f, 2.0f);

            HelpMarker("How much of the frame each half shows."
                           "\n\nA half is half as wide as the frame and just as tall, so the frame"
                           "\ncannot fill it and keep its shape."
                           "\n\nAt 1 the whole frame is there at its right proportions, with bars above"
                           "\nand below. At 2 the half is filled and the sides are cropped away"
                           "\ninstead. Anything between trades one for the other.");
        }

        if (compare == 2)
        {
            float split = config->DlssNrCompareSplit.value_or_default();
            if (ImGui::SliderFloat("Split", &split, 0.0f, 1.0f, "%.2f"))
                config->DlssNrCompareSplit = std::clamp(split, 0.0f, 1.0f);

            HelpMarker("Where the wipe cuts. Left of it is the frame as the upscaler produced it,"
                           "\nright of it is the frame the model edited.");
        }

        static const char* debugNames[] = { "Off", "Proxy (what the model sees)", "Model output (raw)",
                                            "Difference (amplified)" };
        int debugView = (int) config->DlssNrDebugView.value_or_default();
        if (ImGui::Combo("Debug view", &debugView, debugNames, IM_ARRAYSIZE(debugNames)))
            config->DlssNrDebugView = (uint32_t) debugView;

        HelpMarker("Proxy is the picture handed to the model -- if that looks wrong, the white point"
                       "\nis wrong and nothing downstream can be judged."
                       "\n\nDifference shows what the model actually changed, amplified twenty times and"
                       "\ncentred on grey. A flat grey frame there means it is doing nothing.");

        ImGui::PopItemWidth();
    }
}

} // namespace DlssNr

