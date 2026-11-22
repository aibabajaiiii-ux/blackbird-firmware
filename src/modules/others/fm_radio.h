#pragma once

// RDA5807M FM radio module.
// Not a stock Bruce feature -- custom, board-agnostic addition for boards
// that wire an RDA5807M onto an I2C bus (see boards/esp32-devkit-custom).
// If the board doesn't define HAS_FM_RADIO, fm_radio_setup() isn't compiled
// in and the menu entry that calls it isn't added either.

void fm_radio_setup();
