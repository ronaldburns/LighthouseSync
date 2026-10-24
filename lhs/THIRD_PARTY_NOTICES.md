# Third-party notices for LighthouseSync's additions

These come on top of `THIRD_PARTY_NOTICES.md`, which covers what LighthouseSync shares with QuestLHSync.

## Dear ImGui 1.92.9b

The dashboard (`QuestLHSync.exe`) and the headset panel (`lighthousesync-panel`) are built with Dear ImGui, unmodified
from tag `v1.92.9b`, in `third_party/imgui`.

Copyright (c) 2014-2026 Omar Cornut. Licensed under the MIT License; the complete text is in
`third_party/imgui/LICENSE.txt`.

Source: https://github.com/ocornut/imgui

## IBM Plex Sans and IBM Plex Mono

The dashboard and the headset panel have IBM Plex Sans (Regular, Medium, SemiBold) and IBM Plex Mono (Regular) built
in, cut down to Latin characters, from `third_party/fonts`.

Copyright (c) 2017 IBM Corp. with Reserved Font Name "Plex". Licensed under the SIL Open Font License, Version 1.1;
the complete text is in `third_party/fonts/OFL.txt`.

Source: https://github.com/IBM/plex

## QOI

The dashboard and the headset panel send the dashboard's pages to the headset as QOI images, with `qoi.h` from
phoboslab/qoi commit `ffb2d2cb`, unmodified, in `third_party/qoi`.

Copyright (c) 2021 Dominic Szablewski. Licensed under the MIT License; the complete text is in
`third_party/qoi/LICENSE`.

Source: https://github.com/phoboslab/qoi

## Vulkan-Headers 1.4.309

The headset panel is built against the Vulkan headers from KhronosGroup/Vulkan-Headers tag `v1.4.309`, unmodified, in
`third_party/vulkan/include`. It loads the headset's own Vulkan library when it runs.

Copyright (c) 2015-2025 The Khronos Group Inc. Licensed under the Apache License 2.0 or the MIT License, at your
option; LighthouseSync takes them under the MIT License, whose complete text is in `third_party/vulkan/LICENSE`.

Source: https://github.com/KhronosGroup/Vulkan-Headers
