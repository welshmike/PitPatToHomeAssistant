#include "DialMenuView.h"
#if HAS_DIAL_UI

#include "DialGlyphs.h"

namespace
{

constexpr int kMenuCentreX = 120;
constexpr int kMenuCentreY = 120;
constexpr int kMenuNameY   = 120; // highlighted card's name, Font4
constexpr int kMenuHintY   = 146; // "turn - tap"
constexpr int kGlyphR      = 10;
constexpr int kGlyphRHighlight = 12;

const char* cardName(CardId id)
{
    switch (id)
    {
    case CardId::TREADMILL:    return "Treadmill";
    case CardId::CLOCK:        return "Clock";
    case CardId::FLIGHTS:      return "Flights";
    case CardId::LIGHT_OFFICE: return "Office";
    case CardId::LIGHT_LAMP:   return "Lamp";
    case CardId::COUNT:
    default:                   return "";
    }
}

void drawCardGlyph(LovyanGFX& gfx, CardId id, int x, int y, int r, uint32_t colour)
{
    switch (id)
    {
    case CardId::TREADMILL:    drawBeltGlyph(gfx, x, y, r, colour);  break;
    case CardId::CLOCK:        drawClockGlyph(gfx, x, y, r, colour); break;
    case CardId::FLIGHTS:      drawPlaneGlyph(gfx, x, y, r, colour); break;
    case CardId::LIGHT_OFFICE: drawSunGlyph(gfx, x, y, r, colour);   break;
    case CardId::LIGHT_LAMP:   drawBulbGlyph(gfx, x, y, r, colour);  break;
    case CardId::COUNT:
    default:                                                         break;
    }
}

} // namespace

void drawCardMenu(LovyanGFX& gfx, const DialTheme& theme, const CardMenu& menu)
{
    const uint8_t highlight = static_cast<uint8_t>(menu.highlight());

    for (uint8_t i = 0; i < static_cast<uint8_t>(CardId::COUNT); ++i)
    {
        int x = 0;
        int y = 0;
        CardMenu::itemCentre(i, x, y);
        const CardId id = static_cast<CardId>(i);
        if (i == highlight)
        {
            gfx.fillCircle(x, y, CardMenu::kItemRadiusHighlight, theme.col(Col::PENDING));
            drawCardGlyph(gfx, id, x, y, kGlyphRHighlight, theme.col(Col::BG));
        }
        else
        {
            gfx.drawCircle(x, y, CardMenu::kItemRadius, theme.col(Col::DIM));
            drawCardGlyph(gfx, id, x, y, kGlyphR, theme.col(Col::TEXT));
        }
    }

    gfx.setTextDatum(middle_center);
    gfx.setTextColor(theme.col(Col::TEXT), theme.col(Col::BG));
    gfx.drawString(cardName(menu.highlight()), kMenuCentreX, kMenuNameY, &fonts::Font4);
    gfx.setTextColor(theme.col(Col::DIM_DIM), theme.col(Col::BG));
    gfx.drawString("turn - tap", kMenuCentreX, kMenuHintY, &fonts::Font2);
}

#endif // HAS_DIAL_UI
