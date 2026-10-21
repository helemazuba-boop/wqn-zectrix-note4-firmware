# Navigation icons

Seven original 24×24 SVG icons for the global page menu. All use a 2px stroke.
The generated masks are 1bpp, row-major, MSB first; a set bit means paint with
the caller's color. The same mask supplies normal and inverted states.

From the firmware project root:

```sh
python3 tools/ui/generate_navigation_icons.py
python3 tools/ui/generate_navigation_icons.py --check
python3 scripts/test_ui_navigation.py
```

Edit the SVG sources, then regenerate `../navigation_icons.h`. The generator
accepts the circle/polyline subset used here and rejects unsupported geometry.
The seven masks occupy 504 bytes before descriptors and alignment.

The host regression exercises production input reduction, direct page routing,
modal/persist gates, mono composition, gray composition and allocation failure.
It substitutes storage/cloud calls and panel I/O; it does not establish device
refresh latency, ghosting, audio concurrency or sleep/wake behavior.
