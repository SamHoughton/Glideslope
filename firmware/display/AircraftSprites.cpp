#include "display/AircraftSprites.h"

#include "display/SpriteData.h"

namespace
{
    // ── ICAO type designators ────────────────────────────────────────────────
    // Exact codes first, then prefixes. Codes are matched case-insensitively.
    struct TypeRule { const char *code; AircraftSprites::Kind kind; bool prefix; };
    using K = AircraftSprites::Kind;
    const TypeRule kRules[] = {
        // A380 / 747
        {"A38", K::A380, true},
        {"B74", K::B747, true},
        // Business jets (before the generic Embraer/Cessna/Bombardier prefixes)
        {"E35L", K::Bizjet, false}, {"E50P", K::Bizjet, false}, {"E55P", K::Bizjet, false},
        {"E545", K::Bizjet, false}, {"E550", K::Bizjet, false},
        {"C25",  K::Bizjet, true},  {"C500", K::Bizjet, false}, {"C501", K::Bizjet, false},
        {"C510", K::Bizjet, false}, {"C525", K::Bizjet, false}, {"C550", K::Bizjet, false},
        {"C560", K::Bizjet, false}, {"C56X", K::Bizjet, false}, {"C650", K::Bizjet, false},
        {"C680", K::Bizjet, false}, {"C68A", K::Bizjet, false}, {"C700", K::Bizjet, false},
        {"C750", K::Bizjet, false}, {"CL30", K::Bizjet, false}, {"CL35", K::Bizjet, false},
        {"CL60", K::Bizjet, false}, {"GLF",  K::Bizjet, true},  {"GLEX", K::Bizjet, false},
        {"GL5T", K::Bizjet, false}, {"GL7T", K::Bizjet, false}, {"GL8T", K::Bizjet, false},
        {"G280", K::Bizjet, false}, {"FA",   K::Bizjet, true},  {"F2TH", K::Bizjet, false},
        {"F900", K::Bizjet, false}, {"LJ",   K::Bizjet, true},  {"H25",  K::Bizjet, true},
        {"BE40", K::Bizjet, false}, {"PRM1", K::Bizjet, false}, {"PC24", K::Bizjet, false},
        {"HDJT", K::Bizjet, false}, {"SF50", K::Bizjet, false}, {"EA50", K::Bizjet, false},
        {"ASTR", K::Bizjet, false}, {"GALX", K::Bizjet, false},
        // Widebodies
        {"A30",  K::A330, true}, {"A31",  K::A330, true},   // A300/A310 (A318/A319 below)
        {"A33",  K::A330, true}, {"A34",  K::A340, true},
        {"A35",  K::A350, true}, {"B76",  K::B767, true},
        {"B77",  K::B777, true}, {"B78",  K::B787, true},
        {"MD11", K::A330, false}, {"DC10", K::A330, false},
        {"IL96", K::A340, false},
        // Narrowbodies (A318/A319 before the A31 widebody prefix catches them)
        {"A318", K::A320, false}, {"A319", K::A320, false},
        {"A320", K::A320, false}, {"A321", K::A321, false},
        {"A32",  K::A320, true},  {"A19N", K::A320, false},
        {"A20N", K::A320, false}, {"A21N", K::A321, false},
        {"B73",  K::B737, true},  {"B37M", K::B737, false},
        {"B38M", K::B737, false}, {"B39M", K::B737, false},
        {"B3XM", K::B737, false}, {"B75",  K::B757, true},
        {"B712", K::CRJ, false},  {"BCS",  K::A220, true},
        {"MD8",  K::CRJ, true},   {"MD90", K::CRJ, false},
        {"C919", K::A320, false},
        // Regional jets
        {"E17",  K::EJet, true}, {"E19",  K::EJet, true},
        {"E29",  K::EJet, true}, {"E75",  K::EJet, true},
        {"E13",  K::CRJ, true},  {"E14",  K::CRJ, true},
        {"CRJ",  K::CRJ, true},  {"RJ",   K::EJet, true},
        {"B46",  K::EJet, true}, {"SU95", K::EJet, false},
        {"F70",  K::CRJ, false}, {"F100", K::CRJ, false},
        // Turboprops
        {"AT4",  K::ATR, true},   {"AT7",  K::ATR, true},
        {"DH8",  K::Q400, true},   {"DHC6", K::ATR, false},
        {"SF34", K::ATR, false},  {"JS41", K::ATR, false},
        {"JS32", K::ATR, false},  {"D328", K::ATR, false},
        {"SB20", K::ATR, false},  {"F50",  K::ATR, false},
        {"B190", K::ATR, false},  {"BE20", K::ATR, false},
        {"BE9L", K::ATR, false},  {"B350", K::ATR, false},
        {"L410", K::ATR, false},  {"PC12", K::ATR, false},
        {"C208", K::ATR, false},  {"TBM",  K::ATR, true},
        {"P180", K::ATR, false},  {"BN2",  K::ATR, true},
        {"C130", K::ATR, false},  {"A400", K::ATR, false},
        // Helicopters
        {"EC",   K::Helicopter, true},  {"AS3",  K::Helicopter, true},
        {"AS5",  K::Helicopter, true},  {"AS6",  K::Helicopter, true},
        {"A109", K::Helicopter, false}, {"A119", K::Helicopter, false},
        {"A139", K::Helicopter, false}, {"A169", K::Helicopter, false},
        {"A189", K::Helicopter, false}, {"AW",   K::Helicopter, true},
        {"B06",  K::Helicopter, false}, {"B407", K::Helicopter, false},
        {"B412", K::Helicopter, false}, {"B429", K::Helicopter, false},
        {"R22",  K::Helicopter, false}, {"R44",  K::Helicopter, false},
        {"R66",  K::Helicopter, false}, {"S76",  K::Helicopter, false},
        {"S92",  K::Helicopter, false}, {"H60",  K::Helicopter, false},
        {"H160", K::Helicopter, false}, {"H47",  K::Helicopter, false},
        {"NH90", K::Helicopter, false}, {"EH10", K::Helicopter, false},
        {"MI8",  K::Helicopter, false}, {"GAZL", K::Helicopter, false},
        // Light aircraft (pistons)
        {"C15",  K::LightAircraft, true}, {"C17",  K::LightAircraft, true},
        {"C18",  K::LightAircraft, true}, {"C20",  K::LightAircraft, true},
        {"C21",  K::LightAircraft, true}, {"C310", K::LightAircraft, false},
        {"P28",  K::LightAircraft, true}, {"PA",   K::LightAircraft, true},
        {"P32",  K::LightAircraft, true}, {"SR2",  K::LightAircraft, true},
        {"DA4",  K::LightAircraft, true}, {"DA2",  K::LightAircraft, true},
        {"DV20", K::LightAircraft, false}, {"BE3",  K::LightAircraft, true},
        {"BE5",  K::LightAircraft, true}, {"BE76", K::LightAircraft, false},
        {"TB",   K::LightAircraft, true}, {"M20",  K::LightAircraft, true},
        {"AA5",  K::LightAircraft, false}, {"RV",   K::LightAircraft, true},
        {"CH7",  K::LightAircraft, true}, {"C42",  K::LightAircraft, false},
        {"TOBA", K::LightAircraft, false}, {"SIRA", K::LightAircraft, false},
    };

    bool matches(const TypeRule &r, const char *code)
    {
        if (r.prefix) return strncmp(code, r.code, strlen(r.code)) == 0;
        return strcmp(code, r.code) == 0;
    }

    const Rgb kWindow  { 30,  40,  58};
    const Rgb kCockpit { 18,  24,  40};
    const Rgb kWingLit {128, 135, 148};
    const Rgb kWingDark{ 80,  86, 100};
    const Rgb kGear    { 50,  54,  62};
    const Rgb kProp    {130, 135, 145};
    const Rgb kIntake  { 35,  38,  46};

    Rgb mixTo(Rgb a, Rgb b, float k)
    {
        return { (uint8_t)(a.r + (b.r - a.r) * k), (uint8_t)(a.g + (b.g - a.g) * k), (uint8_t)(a.b + (b.b - a.b) * k) };
    }

    // One pixel's colour: the livery with the generator's shading applied.
    bool shade(char ch, const AircraftSprites::Livery &l, Rgb &out)
    {
        switch (ch)
        {
            case '^': out = mixTo(l.body, Rgb{255, 255, 255}, 0.35f); return true;
            case '#': out = FrameCanvas::scale(l.body, 0.88f);   return true;
            case '-': out = FrameCanvas::scale(l.stripe, 0.85f); return true;
            case '+': out = FrameCanvas::scale(l.belly, 0.78f);  return true;
            case '_': out = FrameCanvas::scale(l.belly, 0.55f);  return true;
            // Soft edges: drawn half-blended with what is behind (see draw()).
            case 'a': out = FrameCanvas::scale(l.body, 0.88f);   return true;
            case 'b': out = FrameCanvas::scale(l.belly, 0.78f);  return true;
            case 'r': out = l.tail;                              return true;
            case 't': out = FrameCanvas::scale(l.tail, 0.62f);   return true;
            case 'R': out = l.accent;                            return true;
            case 'e': out = mixTo(l.engine, Rgb{255, 255, 255}, 0.15f); return true;
            case 'E': out = FrameCanvas::scale(l.engine, 0.6f);  return true;
            case 'w': out = kWindow;   return true;
            case 'c': out = kCockpit;  return true;
            case 'g': out = kWingLit;  return true;
            case 'G': out = kWingDark; return true;
            case 'd': out = kGear;     return true;
            case 'p': out = kProp;     return true;
            case 'i': out = kIntake;   return true;
            default:  return false;
        }
    }

    const AircraftSprites::Sprite &entry(const SpriteData::Entry *table, AircraftSprites::Kind k)
    {
        // SpriteData::Entry has the same layout as Sprite.
        static_assert(sizeof(SpriteData::Entry) == sizeof(AircraftSprites::Sprite), "sprite tables");
        return reinterpret_cast<const AircraftSprites::Sprite &>(table[k < AircraftSprites::KindCount ? k : 0]);
    }
}

const AircraftSprites::Sprite &AircraftSprites::get(Kind k)      { return entry(SpriteData::kCard, k); }
const AircraftSprites::Sprite &AircraftSprites::getScene(Kind k) { return entry(SpriteData::kScene, k); }

AircraftSprites::Livery AircraftSprites::liveryFor(const FlightInfo &f, Rgb accent)
{
    // The airline actually flying it: the call sign's ICAO prefix first.
    char codes[3][4] = {};
    auto prefix = [](const String &s, char *out) {
        if (s.length() >= 3 && isalpha((unsigned char)s[0]) && isalpha((unsigned char)s[1]) && isalpha((unsigned char)s[2]))
            for (int i = 0; i < 3; ++i) out[i] = (char)toupper((unsigned char)s[i]);
    };
    prefix(f.ident_icao.length() ? f.ident_icao : f.ident, codes[0]);
    prefix(f.logo_code, codes[1]);
    prefix(f.operator_icao, codes[2]);
    for (auto &code : codes)
    {
        if (!code[0]) continue;
        for (const auto &a : SpriteData::kAliases)
            if (strcmp(a[0], code) == 0) { strlcpy(code, a[1], 4); break; }
        for (const SpriteData::LiveryRow &r : SpriteData::kLiveries)
            if (strcmp(r.code, code) == 0)
            {
                Livery l;
                Rgb *parts[] = {&l.body, &l.belly, &l.stripe, &l.tail, &l.accent, &l.engine};
                for (int i = 0; i < 6; ++i) *parts[i] = Rgb{r.rgb[i][0], r.rgb[i][1], r.rgb[i][2]};
                return l;
            }
    }
    return Livery(accent);
}
AircraftSprites::Kind AircraftSprites::classify(const String &icaoType, bool &known)
{
    // Trimmed, upper-cased copy in a stack buffer (called every frame).
    char code[8];
    size_t n = 0;
    for (size_t i = 0; i < icaoType.length() && n + 1 < sizeof(code); ++i)
        if (!isspace((unsigned char)icaoType[i])) code[n++] = (char)toupper((unsigned char)icaoType[i]);
    code[n] = '\0';
    // Exact matches win over prefixes, so A318/A319 aren't caught by "A31".
    for (const TypeRule &r : kRules)
        if (!r.prefix && matches(r, code)) { known = true; return r.kind; }
    for (const TypeRule &r : kRules)
        if (r.prefix && matches(r, code)) { known = true; return r.kind; }
    known = false;
    return A320;
}

void AircraftSprites::draw(FrameCanvas &c, const Sprite &s, int x, int y, const Livery &livery,
                           bool flipX, bool grey, bool outline)
{
    auto at = [&](int r, int col) -> char {
        if (r < 0 || r >= s.h || col < 0 || col >= s.w) return '.';
        return s.rows[r][flipX ? s.w - 1 - col : col];
    };

    // Outline: a solid black backing so whatever the sprite passes over can't
    // show through it. Each row is cleared from the leftmost to the rightmost
    // pixel of the sprite in that row and its neighbours (1px halo all round),
    // which also fills the gaps between engines, wings and tail.
    if (outline)
        for (int r = -1; r <= s.h; ++r)
        {
            int first = s.w, last = -1;
            for (int dy = -1; dy <= 1; ++dy)
                for (int col = 0; col < s.w; ++col)
                    if (at(r + dy, col) != '.')
                    {
                        first = min(first, col);
                        last  = max(last, col);
                    }
            for (int col = first - 1; col <= last + 1; ++col)
                c.set(x + col, y + r, (uint16_t)0);
        }

    const Livery greys{};   // unknown type: the default colours, in grey
    for (int r = 0; r < s.h; ++r)
        for (int col = 0; col < s.w; ++col)
        {
            Rgb px;
            if (!shade(at(r, col), grey ? greys : livery, px)) continue;
            if (grey)
            {
                const uint8_t v = (uint8_t)((px.r * 3 + px.g * 4 + px.b) / 8 * 0.75f);
                px = Rgb{v, v, (uint8_t)min(255, v + 6)};
            }
            const char ch = at(r, col);
            if (ch == 'a' || ch == 'b')
                px = mixTo(FrameCanvas::unpack(c.get(x + col, y + r)), px, 0.55f);
            c.set(x + col, y + r, px);
        }
}
