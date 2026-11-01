/**
 * rrc_tiny.h — the RRC status page on the tiny OLED.
 *
 * The when:-gated tinylcd slice of rrc (conditional/tinylcd/): compiled only
 * when spangap/tinylcd is staged. One page: the connected hubs as dots, the
 * unread total, the mention count and where the newest mention is. See
 * rrc_tiny.cpp.
 */
#pragma once

#include "service.h"

class RrcTinyPage : public Service {
public:
    void onInit() override;
};
