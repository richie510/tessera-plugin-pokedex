#include "pokedex.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include "esphome/core/helpers.h"

namespace esphome::pokedex {

using tessera::Font;
namespace ui = tessera::ui;

// Where the entries and the pictures come from: this repository's dex/<n>.json (the manifest's fetch `entry`)
// and PokéAPI's sprites on GitHub. Both hosts are raw.githubusercontent.com, the manifest's permissions.network.
static const char *const PIXEL = "https://raw.githubusercontent.com/PokeAPI/sprites/master/sprites/pokemon/%d.png";
static const char *const ARTWORK =
    "https://raw.githubusercontent.com/PokeAPI/sprites/master/sprites/pokemon/other/official-artwork/%d.png";

static const char *const STAT_KEYS[6] = {"hp", "atk", "def", "spa", "spd", "spe"};
static constexpr int STAT_MAX = 255;     // the highest base stat any Pokémon has
static constexpr uint32_t RETRY_S = 60;  // after an ask that failed

// The highest dex number of each pool (the manifest's option `pool`).
static int pool_last(const char *pool) {
  if (!std::strcmp(pool, "gen2")) return 251;
  if (!std::strcmp(pool, "gen3")) return 386;
  if (!std::strcmp(pool, "all")) return 1025;
  return 151;
}

// Days since 1970-01-01 for a date (Howard Hinnant's days_from_civil), for the Pokémon of the day.
static long days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long>(doe) - 719468;
}

// A number as the entry writes it: 0.4, 6, 100.5.
static std::string decimal(float value) {
  char out[16];
  if (std::fabs(value - std::round(value)) < 0.05f) std::snprintf(out, sizeof(out), "%d", static_cast<int>(std::round(value)));
  else std::snprintf(out, sizeof(out), "%.1f", value);
  return out;
}

// The screensaver: a step of its own, shown while its entity is on. It picks a Pokémon (a new one
// every few minutes, one a day, or always the same), asks the app for its entry (the fetch `entry`, plugin API 0.27),
// and draws its picture on the left (on top of a glass that stands up) with the words and the stats beside it.
//
// A saver is drawn in the dark look on the black glass whatever the screen's own look, so its roles are white on black.
// The screen moves the whole of it a millimetre every minute (the manifest has no `moves: true`). show() makes every
// part once; an answer paints the words, the bars and the picture; on_tick() only decides when to ask for the next.
class DexSaver : public tessera::Saver {
 public:
  explicit DexSaver(Pokedex *plugin) : plugin_(plugin) { plugin_->attach(this); }
  ~DexSaver() override { plugin_->detach(this); }

  void show(const tessera::SaverContext &c) override {
    // An option the person never touched may not be in the word: read each with its default.
    const char *mode = c.options["mode"] | "random";
    mode_ = !std::strcmp(mode, "daily") ? Mode::DAILY : !std::strcmp(mode, "pick") ? Mode::PICK : Mode::RANDOM;
    // Numbers may come as 5 or 5.0: read them as floats.
    every_s_ = static_cast<uint32_t>(c.options["every"] | 5.0f) * 60;
    if (every_s_ < 60) every_s_ = 60;
    pick_ = static_cast<int>(c.options["pokemon"] | 25.0f);
    if (pick_ < 1 || pick_ > 1025) pick_ = 25;
    last_ = pool_last(c.options["pool"] | "gen1");
    artwork_ = c.options["artwork"] | true;
    dex_text_ = c.options["dex_text"] | true;
    stats_ = c.options["stats"] | true;
    lay_out(c);
    ask(next_number(tessera::epoch()));
  }

  void on_tick(uint32_t epoch) override {
    if (!epoch) return;  // the screen's clock is not set yet: keep what is on the glass
    if (!started_) {     // the first second with a clock: count from here
      started_ = epoch;
      day_ = today(epoch);
    }
    if (failed_ && epoch - failed_at_ >= RETRY_S) {
      ask(number_ ? number_ : next_number(epoch));
      return;
    }
    if (mode_ == Mode::RANDOM && epoch - started_ >= every_s_) {
      started_ = epoch;
      ask(next_number(epoch));
    } else if (mode_ == Mode::DAILY && today(epoch) != day_) {
      day_ = today(epoch);
      ask(next_number(epoch));
    }
  }

  // The answer of the app to an ask (Pokedex::on_message): {"re", "ok": true, "result": {name, genus, ...}}, or
  // {"re", "ok": false, "error"}.
  void answer(JsonObjectConst message) {
    if ((message["re"] | 0u) != asked_ || !asked_) return;
    asked_ = 0;
    if (!(message["ok"] | false)) {
      failed_ = true;
      failed_at_ = tessera::epoch();
      // Keep a Pokémon that is already on the glass; say so only over an empty one.
      if (!shown_) ui::set_text(name_, plugin_->text("failed"));
      return;
    }
    failed_ = false;
    shown_ = true;
    paint(message["result"]);
  }

  void on_picture(lv_obj_t *place, tessera::PictureState state) override {
    // Artwork that does not come: the pixel sprite every Pokémon has.
    if (place == picture_ && state != tessera::PictureState::SHOWN && artwork_ && !fell_back_ && number_) {
      fell_back_ = true;
      ui::set_picture(picture_, url(PIXEL, number_), tessera::Fit::CONTAIN);
    }
  }

 private:
  enum class Mode : uint8_t { RANDOM, DAILY, PICK };

  // Every part, once. A square glass (the 4-inch boards) gets a card of its own, everything centred under a large
  // picture; any other glass the picture on the left of one that lies (on top of one that stands), the words the rest.
  void lay_out(const tessera::SaverContext &c) {
    width_ = c.width;
    if (ui::pictures() && std::abs(c.width - c.height) * 5 < std::min(c.width, c.height)) {
      lay_out_card(c);
      return;
    }
    const int margin = ui::mm(4), gap = ui::mm(5);
    const bool upright = c.height > c.width;
    int x = margin, y = margin, w = c.width - 2 * margin, h = c.height - 2 * margin;
    if (ui::pictures()) {
      picture_ = ui::picture(c.parent, tessera::Placeholder::NONE);
      int side;
      if (upright) {
        side = std::min(w, stats_ ? h * 2 / 5 : h / 2);
        lv_obj_set_pos(picture_, (c.width - side) / 2, y);
        y += side + gap;
        h -= side + gap;
      } else {
        side = std::min(h, stats_ ? w * 2 / 5 : w / 2);
        lv_obj_set_pos(picture_, x, (c.height - side) / 2);
        x += side + gap;
        w -= side + gap;
      }
      lv_obj_set_size(picture_, side, side);
    }

    // The column of words: number, name, kind, types, the six stats, height and weight, and the dex text in what is left.
    const int body = ui::line_height(Font::BODY), title = ui::line_height(Font::TITLE);
    const int headline = ui::line_height(Font::HEADLINE), small_gap = ui::px(6);
    const int badge_h = body + ui::px(8), row_h = body + ui::px(6);
    const int fixed = title + headline + body + small_gap + badge_h + small_gap + (stats_ ? 6 * row_h : 0) + small_gap + body;
    const int dex_max = dex_text_ ? 4 * body : 0;
    const int dex_h = dex_text_ ? std::max(0, std::min(dex_max, h - fixed - small_gap)) : 0;
    const int used = fixed + (dex_h > 0 ? small_gap + dex_h : 0);
    int top = y + std::max(0, (h - used) / 2);

    number_label_ = line(c.parent, Font::TITLE, theme::MUTED, x, top, w);
    top += title;
    name_ = line(c.parent, Font::HEADLINE, theme::INK, x, top, w);
    ui::set_text(name_, plugin_->text("waiting"));
    top += headline;
    genus_ = line(c.parent, Font::BODY, theme::MUTED, x, top, w);
    top += body + small_gap;

    make_badges(c.parent, x, top, badge_h, body);
    top += badge_h + small_gap;

    // Stats, unless the person turned them off: the name, a bar over its track, the number at the end.
    if (stats_) {
      int name_w = 0;
      for (const char *key : STAT_KEYS) name_w = std::max(name_w, ui::text_width(plugin_->text(key), Font::BODY));
      const int value_w = ui::text_width("888", Font::BODY);
      track_w_ = std::max(ui::px(20), w - name_w - value_w - ui::px(20));
      for (int i = 0; i < 6; ++i) {
        make_stat(c.parent, i, x, top, name_w, value_w, body);
        top += row_h;
      }
    }
    top += small_gap;
    size_ = line(c.parent, Font::BODY, theme::MUTED, x, top, w);
    top += body + small_gap;
    if (dex_h > 0) {
      dex_ = ui::text(c.parent, Font::BODY, theme::INK_SOFT);
      lv_obj_set_size(dex_, w, dex_h);
      lv_obj_set_pos(dex_, x, top);
    }
  }

  // A square glass: the picture large and centred on top; the name with its number, the kind with the size, and the
  // types centred under it; the six stats in two columns of three (unless turned off: the picture grows instead); the
  // dex text centred at the foot. paint() centres
  // the rows whose width depends on the entry.
  void lay_out_card(const tessera::SaverContext &c) {
    card_ = true;
    const int margin = ui::mm(4), w = c.width - 2 * margin;
    const int body = ui::line_height(Font::BODY), title = ui::line_height(Font::TITLE);
    const int headline = ui::line_height(Font::HEADLINE), small_gap = ui::px(8);
    const int badge_h = body + ui::px(8), row_h = body + ui::px(6);
    const int words = headline + body + small_gap + badge_h + (stats_ ? small_gap + 3 * row_h : 0);
    // Without the stats the picture takes their room too, leaving three lines for the dex text.
    const int h = c.height - 2 * margin;
    const int roomy = h - words - (dex_text_ ? small_gap + 3 * body : 0);
    const int side = std::max(ui::mm(10), std::min(w, stats_ ? h * 2 / 5 : std::min(roomy, h * 3 / 5)));
    // The dex text in what is left, up to three lines centred; a longer entry ends with dots.
    const int left = c.height - 2 * margin - side - words - small_gap;
    const int dex_h = dex_text_ && left >= body ? std::min(3 * body, left) : 0;
    const int used = side + words + (dex_h ? small_gap + dex_h : 0);
    int top = margin + std::max(0, (c.height - 2 * margin - used) / 2);

    picture_ = ui::picture(c.parent, tessera::Placeholder::NONE);
    lv_obj_set_size(picture_, side, side);
    lv_obj_set_pos(picture_, (c.width - side) / 2, top);
    top += side;

    name_y_ = top;
    name_ = line(c.parent, Font::HEADLINE, theme::INK, margin, top, w);
    lv_obj_set_style_text_align(name_, LV_TEXT_ALIGN_CENTER, 0);
    ui::set_text(name_, plugin_->text("waiting"));
    // The number beside the name, on its baseline; paint() places both.
    number_label_ = line(c.parent, Font::TITLE, theme::MUTED, margin, top + headline - title - ui::px(2), 1);
    top += headline;
    genus_ = line(c.parent, Font::BODY, theme::MUTED, margin, top, w);
    lv_obj_set_style_text_align(genus_, LV_TEXT_ALIGN_CENTER, 0);
    top += body + small_gap;

    make_badges(c.parent, margin, top, badge_h, body);
    top += badge_h;
    if (stats_) top += small_gap + lay_out_columns(c.parent, margin, top + small_gap, w, body, row_h);
    top += small_gap;

    if (dex_h > 0) {
      dex_ = ui::text(c.parent, Font::BODY, theme::INK_SOFT);
      lv_obj_set_size(dex_, w, dex_h);
      lv_obj_set_pos(dex_, margin, top);
      lv_obj_set_style_text_align(dex_, LV_TEXT_ALIGN_CENTER, 0);
    }
  }

  // Two columns of three stats, a gap between them, from `top`; returns their height.
  int lay_out_columns(lv_obj_t *parent, int margin, int top, int w, int body, int row_h) {
    const int column_gap = ui::mm(5), column_w = (w - column_gap) / 2;
    int name_w = 0;
    for (const char *key : STAT_KEYS) name_w = std::max(name_w, ui::text_width(plugin_->text(key), Font::BODY));
    const int value_w = ui::text_width("888", Font::BODY);
    track_w_ = std::max(ui::px(20), column_w - name_w - value_w - ui::px(20));
    for (int i = 0; i < 6; ++i) {
      const int x = margin + (i < 3 ? 0 : column_w + column_gap);
      make_stat(parent, i, x, top + (i % 3) * row_h, name_w, value_w, body);
    }
    return 3 * row_h;
  }

  void make_badges(lv_obj_t *parent, int x, int y, int badge_h, int body) {
    for (int i = 0; i < 2; ++i) {
      badge_[i] = ui::block(parent, theme::TRACK);
      lv_obj_set_size(badge_[i], 1, badge_h);
      lv_obj_set_pos(badge_[i], x, y);
      lv_obj_add_flag(badge_[i], LV_OBJ_FLAG_HIDDEN);
      badge_text_[i] = ui::label(badge_[i], Font::BODY, theme::INK);
      lv_obj_set_pos(badge_text_[i], ui::px(10), (badge_h - body) / 2);
    }
    badge_x_ = x;
    badge_y_ = y;
    badge_h_ = badge_h;
  }

  // One stat: its name, a bar over its track, the number at the end.
  void make_stat(lv_obj_t *parent, int i, int x, int y, int name_w, int value_w, int body) {
    const int bar_x = x + name_w + ui::px(10), bar_h = std::max(ui::px(6), body / 3);
    lv_obj_t *label = line(parent, Font::BODY, theme::MUTED, x, y, name_w);
    ui::set_text(label, plugin_->text(STAT_KEYS[i]));
    lv_obj_t *track = ui::block(parent, theme::TRACK);
    lv_obj_set_size(track, track_w_, bar_h);
    lv_obj_set_pos(track, bar_x, y + (body - bar_h) / 2);
    bar_[i] = ui::block(parent, theme::ACCENT);
    lv_obj_set_size(bar_[i], 1, bar_h);
    lv_obj_set_pos(bar_[i], bar_x, y + (body - bar_h) / 2);
    lv_obj_add_flag(bar_[i], LV_OBJ_FLAG_HIDDEN);
    value_[i] = line(parent, Font::BODY, theme::INK, bar_x + track_w_ + ui::px(10), y, value_w);
    lv_obj_set_style_text_align(value_[i], LV_TEXT_ALIGN_RIGHT, 0);
  }

  static lv_obj_t *line(lv_obj_t *parent, Font font, theme::Role role, int x, int y, int w) {
    lv_obj_t *label = ui::label(parent, font, role);
    lv_obj_set_width(label, w);
    lv_obj_set_pos(label, x, y);
    return label;
  }

  // The words, the bars and the picture of one entry.
  void paint(JsonObjectConst r) {
    char number[8];
    std::snprintf(number, sizeof(number), "#%03d", number_);
    ui::set_text(number_label_, number);
    const char *name = r["name"] | "";
    ui::set_text(name_, name);
    const float height = r["height"] | 0.0f, weight = r["weight"] | 0.0f;
    std::string size;
    if (height > 0 || weight > 0) {
      size = tessera::fill(plugin_->text("size"), "height", decimal(height));
      size = tessera::fill(size.c_str(), "weight", decimal(weight));
    }
    const char *genus = r["genus"] | "";
    if (card_) {
      // The name and its number as one centred row; the kind and the size on one line under it.
      const int nw = ui::text_width(name, Font::HEADLINE), mw = ui::text_width(number, Font::TITLE);
      const int between = ui::px(10), room = width_ - 2 * ui::mm(4) - mw - between;
      const int shown = std::min(nw, room), x = (width_ - shown - between - mw) / 2;
      lv_obj_set_style_text_align(name_, LV_TEXT_ALIGN_LEFT, 0);
      lv_obj_set_width(name_, shown);
      lv_obj_set_pos(name_, x, name_y_);
      lv_obj_set_width(number_label_, mw);
      lv_obj_set_x(number_label_, x + shown + between);
      std::string under = genus;
      if (!size.empty()) under += (under.empty() ? "" : ", ") + size;
      ui::set_text(genus_, under);
    } else {
      ui::set_text(genus_, genus);
    }

    const char *types[2] = {r["type1"] | "", r["type2"] | ""};
    int bx = badge_x_;
    if (card_) {  // the badges centred together
      int total = -ui::px(8);
      for (const char *type : types)
        if (*type) total += ui::text_width(type, Font::BODY) + ui::px(28);
      bx = (width_ - total) / 2;
    }
    for (int i = 0; i < 2; ++i) {
      if (!*types[i]) {
        lv_obj_add_flag(badge_[i], LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      const int tw = ui::text_width(types[i], Font::BODY);
      ui::set_text(badge_text_[i], types[i]);
      lv_obj_set_width(badge_text_[i], tw);
      lv_obj_set_size(badge_[i], tw + ui::px(20), badge_h_);
      lv_obj_set_pos(badge_[i], bx, badge_y_);  // our own y: LVGL reads 0 until it lays out
      lv_obj_remove_flag(badge_[i], LV_OBJ_FLAG_HIDDEN);
      bx += tw + ui::px(20) + ui::px(8);
    }

    JsonArrayConst stats = r["stats"];
    for (int i = 0; i < 6 && bar_[i]; ++i) {
      const int v = static_cast<int>(stats[i] | 0.0f);  // a number may come as 35 or 35.0
      ui::set_text(value_[i], v ? std::to_string(v) : "");
      const int fill = std::max(ui::px(3), track_w_ * std::min(v, STAT_MAX) / STAT_MAX);
      lv_obj_set_width(bar_[i], fill);
      if (v) lv_obj_remove_flag(bar_[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(bar_[i], LV_OBJ_FLAG_HIDDEN);
    }

    if (size_) ui::set_text(size_, size);

    if (dex_) {
      std::string text;
      for (const char *key : {"dex0", "dex1", "dex2", "dex3"}) {
        const char *part = r[key] | "";
        if (!*part) continue;
        if (!text.empty()) text += ' ';
        text += part;
      }
      ui::set_text(dex_, text);
    }

    if (picture_) {
      fell_back_ = false;
      ui::set_picture(picture_, url(artwork_ ? ARTWORK : PIXEL, number_), tessera::Fit::CONTAIN);
    }
  }

  static std::string url(const char *pattern, int number) {
    char out[128];
    std::snprintf(out, sizeof(out), pattern, number);
    return out;
  }

  // The day on the screen's own calendar, for the Pokémon of the day.
  static long today(uint32_t epoch) {
    const tessera::LocalTime now = tessera::local_time(epoch);
    return days_from_civil(now.year, static_cast<unsigned>(now.month), static_cast<unsigned>(now.day));
  }

  int next_number(uint32_t epoch) {
    switch (mode_) {
      case Mode::PICK:
        return pick_;
      case Mode::DAILY: {
        // The same Pokémon all day on every screen, in an order that jumps around the pool. Until the clock is set,
        // the first of the pool.
        if (!epoch) return 1;
        const uint32_t day = static_cast<uint32_t>(today(epoch));
        return 1 + static_cast<int>((day * 2654435761u) % static_cast<uint32_t>(last_));
      }
      default: {
        int n = 1 + static_cast<int>(random_uint32() % static_cast<uint32_t>(last_));
        if (n == number_ && last_ > 1) n = n % last_ + 1;  // never the same one twice in a row
        return n;
      }
    }
  }

  // Ask the app for one entry: the fetch `entry` with {n}, answered in on_message.
  void ask(int number) {
    number_ = number;
    failed_ = false;
    JsonDocument request;
    request["ask"] = "fetch:entry";
    request["data"]["n"] = number;
    asked_ = tessera::send(plugin_, request.as<JsonObjectConst>());
    if (!asked_) {  // no app right now: try again in a minute
      failed_ = true;
      failed_at_ = tessera::epoch();
    }
  }

  Pokedex *plugin_;
  Mode mode_ = Mode::RANDOM;
  uint32_t every_s_ = 300, started_ = 0, failed_at_ = 0, asked_ = 0;
  long day_ = 0;
  int pick_ = 25, last_ = 151, number_ = 0, track_w_ = 0, badge_x_ = 0, badge_y_ = 0, badge_h_ = 0, width_ = 0, name_y_ = 0;
  bool card_ = false, artwork_ = true, dex_text_ = true, stats_ = true, failed_ = false, shown_ = false, fell_back_ = false;
  lv_obj_t *picture_{}, *number_label_{}, *name_{}, *genus_{}, *size_{}, *dex_{};
  lv_obj_t *badge_[2]{}, *badge_text_[2]{}, *bar_[6]{}, *value_[6]{};
};

void Pokedex::setup() {
  // The id is the saver's id in the manifest, and its step must be the manifest's (own).
  add_saver("dex", tessera::SaverStep::OWN, [this]() { return new DexSaver(this); });
}

void Pokedex::on_message(JsonObjectConst message) {
  if (shown_) shown_->answer(message);
}

}  // namespace esphome::pokedex
