#pragma once

// Test-authored 5x7 bitmap font, hard edges, no platform font/rasterizer dependency.
// Rectangles below are also the fixed analysis ROIs in analyze_hud.py (1280x720).
static void HudFixture(std::vector<unsigned char>& pixels, unsigned width, unsigned height, unsigned frame)
{
    auto pixel = [&](int x, int y, unsigned rgb, unsigned alpha = 255)
    {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height))
            return;
        auto index = (static_cast<size_t>(y) * width + x) * 4;
        for (unsigned c = 0; c < 3; ++c)
        {
            unsigned value = (rgb >> (16 - 8 * c)) & 255;
            pixels[index + c] =
                static_cast<unsigned char>((value * alpha + pixels[index + c] * (255 - alpha) + 127) / 255);
        }
    };
    auto rect = [&](int x, int y, int w, int h, unsigned rgb, unsigned alpha = 255)
    {
        for (int j = y; j < y + h; ++j)
            for (int i = x; i < x + w; ++i)
                pixel(i, j, rgb, alpha);
    };
    auto glyph = [](char c) -> std::array<unsigned, 7>
    {
        switch (c)
        {
        case '0':
            return { 14, 17, 19, 21, 25, 17, 14 };
        case '1':
            return { 4, 12, 4, 4, 4, 4, 14 };
        case '2':
            return { 14, 17, 1, 2, 4, 8, 31 };
        case '3':
            return { 30, 1, 1, 14, 1, 1, 30 };
        case '4':
            return { 2, 6, 10, 18, 31, 2, 2 };
        case '5':
            return { 31, 16, 16, 30, 1, 1, 30 };
        case '6':
            return { 14, 16, 16, 30, 17, 17, 14 };
        case '7':
            return { 31, 1, 2, 4, 8, 8, 8 };
        case '8':
            return { 14, 17, 17, 14, 17, 17, 14 };
        case '9':
            return { 14, 17, 17, 15, 1, 1, 14 };
        case 'A':
            return { 14, 17, 17, 31, 17, 17, 17 };
        case 'E':
            return { 31, 16, 16, 30, 16, 16, 31 };
        case 'H':
            return { 17, 17, 17, 31, 17, 17, 17 };
        case 'I':
            return { 14, 4, 4, 4, 4, 4, 14 };
        case 'L':
            return { 16, 16, 16, 16, 16, 16, 31 };
        case 'M':
            return { 17, 27, 21, 21, 17, 17, 17 };
        case 'N':
            return { 17, 25, 25, 21, 19, 19, 17 };
        case 'O':
            return { 14, 17, 17, 17, 17, 17, 14 };
        case 'P':
            return { 30, 17, 17, 30, 16, 16, 16 };
        case 'R':
            return { 30, 17, 17, 30, 20, 18, 17 };
        case 'S':
            return { 15, 16, 16, 14, 1, 1, 30 };
        case 'T':
            return { 31, 4, 4, 4, 4, 4, 4 };
        case 'U':
            return { 17, 17, 17, 17, 17, 17, 14 };
        case 'X':
            return { 17, 17, 10, 4, 10, 17, 17 };
        case ':':
            return { 0, 4, 4, 0, 4, 4, 0 };
        default:
            return {};
        }
    };
    auto text = [&](int x, int y, const std::string& label, int scale, unsigned rgb)
    {
        for (char c : label)
        {
            auto rows = glyph(c);
            for (int row = 0; row < 7; ++row)
                for (int col = 0; col < 5; ++col)
                    if (rows[row] & (1u << (4 - col)))
                        rect(x + col * scale, y + row * scale, scale, scale, rgb);
            x += 6 * scale;
        }
    };
    rect(32, 112, 370, 64, 0x080808);
    text(40, 120, "HEALTH: 100 AMMO: 42", 2, 0xFFFFFF);
    text(40, 150, "0123456789 SMALL TEXT", 1, 0xFFFF00);
    // Panel vanishes at 16 and reappears at 24 without Reset: a disappearance/halo probe.
    if (frame < 16 || frame >= 24)
    {
        rect(420, 180, 480, 290, 0x101820, 150);
        text(444, 204, "MENU", 4, 0xFFFFFF);
        text(444, 268, "RESUME", 2, 0xFFFFFF);
        text(444, 302, "OPTIONS", 2, 0xD0D0D0);
        rect(436, 334, 448, 42, 0x80C0FF, 90);
        text(444, 344, "MAP", 2, 0xFFFFFF);
    }
    // One-pixel minimap roads with small high-contrast points.
    rect(930, 80, 310, 260, 0x101010, 200);
    for (int y = 96; y < 328; ++y)
    {
        pixel(948 + (y - 96) / 2, y, 0xFFFFFF);
        pixel(1198 - (y - 96) / 3, y, 0x70FF70);
    }
    for (int x = 942; x < 1228; ++x)
        pixel(x, 220, 0xFFFFFF);
    for (int x = 960; x < 1210; x += 31)
        rect(x, 180, 2, 2, 0xFFFF00);
    // Teleport a bright outlined icon + change text at frame 8; return at 24.
    const int x = frame < 8 || frame >= 24 ? 60 : 330;
    rect(x, 560, 80, 65, 0xFFFFFF);
    rect(x + 1, 561, 78, 63, 0x000000);
    text(x + 10, 576, frame < 8 || frame >= 24 ? "100" : "075", 3, 0xFFFFFF);
}
