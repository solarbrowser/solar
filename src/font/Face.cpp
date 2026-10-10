#include "solar/font/Face.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <zlib.h>

#include <hb-ot.h>
#include <hb.h>

namespace solar::font {

namespace {

uint32_t Be32(const std::string& s, size_t at) {
  if (at + 4 > s.size()) return 0;
  return (static_cast<uint32_t>(static_cast<uint8_t>(s[at])) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[at + 2])) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(s[at + 3]));
}

void Put32(std::string& s, uint32_t v) {
  s.push_back(static_cast<char>(v >> 24));
  s.push_back(static_cast<char>(v >> 16));
  s.push_back(static_cast<char>(v >> 8));
  s.push_back(static_cast<char>(v));
}

void Put16(std::string& s, uint16_t v) {
  s.push_back(static_cast<char>(v >> 8));
  s.push_back(static_cast<char>(v));
}

// A WOFF file as the sfnt it wraps (https://www.w3.org/TR/WOFF/); empty if it is not a good one.
std::string UnwrapWoff(const std::string& woff) {
  if (woff.size() < 44 || woff.compare(0, 4, "wOFF") != 0) return "";
  const uint32_t flavor = Be32(woff, 4);
  const uint32_t tables = (static_cast<uint8_t>(woff[12]) << 8) | static_cast<uint8_t>(woff[13]);
  struct Entry {
    uint32_t tag, offset, compressed, original, checksum;
  };
  std::vector<Entry> entries;
  for (uint32_t i = 0; i < tables; ++i) {
    const size_t at = 44 + i * 20;
    if (at + 20 > woff.size()) return "";
    entries.push_back({Be32(woff, at), Be32(woff, at + 4), Be32(woff, at + 8), Be32(woff, at + 12), Be32(woff, at + 16)});
  }
  std::string out;
  Put32(out, flavor);
  Put16(out, static_cast<uint16_t>(tables));
  uint16_t searchRange = 1, entrySelector = 0;
  while (searchRange * 2 <= tables) {
    searchRange *= 2;
    ++entrySelector;
  }
  Put16(out, static_cast<uint16_t>(searchRange * 16));
  Put16(out, entrySelector);
  Put16(out, static_cast<uint16_t>(tables * 16 - searchRange * 16));
  std::vector<std::string> bodies;
  for (const Entry& e : entries) {
    if (static_cast<uint64_t>(e.offset) + e.compressed > woff.size()) return "";
    std::string body;
    if (e.compressed < e.original) {
      body.resize(e.original);
      uLongf size = e.original;
      if (uncompress(reinterpret_cast<Bytef*>(body.data()), &size, reinterpret_cast<const Bytef*>(woff.data() + e.offset), e.compressed) != Z_OK || size != e.original) return "";
    } else {
      body = woff.substr(e.offset, e.original);
    }
    bodies.push_back(std::move(body));
  }
  size_t offset = 12 + tables * 16;
  for (size_t i = 0; i < entries.size(); ++i) {
    Put32(out, entries[i].tag);
    Put32(out, entries[i].checksum);
    Put32(out, static_cast<uint32_t>(offset));
    Put32(out, entries[i].original);
    offset += (bodies[i].size() + 3) & ~size_t(3);
  }
  for (const std::string& body : bodies) {
    out += body;
    out.append((4 - body.size() % 4) % 4, '\0');
  }
  return out;
}

std::string Tag(hb_tag_t tag) {
  char text[4];
  hb_tag_to_string(tag, text);
  return std::string(text, 4);
}

std::string NameOf(hb_face_t* face, hb_ot_name_id_t id) {
  unsigned size = 0;
  size = hb_ot_name_get_utf8(face, id, HB_LANGUAGE_INVALID, nullptr, nullptr);
  if (size == 0) return "";
  std::string text(size + 1, '\0');
  unsigned capacity = size + 1;
  hb_ot_name_get_utf8(face, id, HB_LANGUAGE_INVALID, &capacity, text.data());
  text.resize(capacity);
  return text;
}

}  // namespace

struct Face::Impl {
  hb_blob_t* blob = nullptr;
  hb_face_t* face = nullptr;
  hb_font_t* font = nullptr;  // at units per em, so that positions are in font units
  unsigned upem = 1000;
};

Face::~Face() {
  if (!impl_) return;
  if (impl_->font) hb_font_destroy(impl_->font);
  if (impl_->face) hb_face_destroy(impl_->face);
  if (impl_->blob) hb_blob_destroy(impl_->blob);
}

unsigned Face::CountIn(const std::string& data) {
  hb_blob_t* blob = hb_blob_create(data.data(), static_cast<unsigned>(data.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  const unsigned count = hb_face_count(blob);
  hb_blob_destroy(blob);
  return count;
}

std::shared_ptr<Face> Face::Open(std::string data, unsigned index) {
  if (data.size() >= 4 && data.compare(0, 4, "wOFF") == 0) data = UnwrapWoff(data);
  if (data.size() < 12) return nullptr;
  std::shared_ptr<Face> result(new Face());
  result->data_ = std::move(data);
  result->impl_ = std::make_unique<Impl>();
  result->impl_->blob = hb_blob_create(result->data_.data(), static_cast<unsigned>(result->data_.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  result->impl_->face = hb_face_create(result->impl_->blob, index);
  return Finish(std::move(result));
}

std::shared_ptr<Face> Face::OpenFile(const std::string& path, unsigned index) {
  hb_blob_t* blob = hb_blob_create_from_file_or_fail(path.c_str());
  if (!blob) return nullptr;
  std::shared_ptr<Face> result(new Face());
  result->impl_ = std::make_unique<Impl>();
  result->impl_->blob = blob;
  result->impl_->face = hb_face_create(blob, index);
  return Finish(std::move(result));
}

std::shared_ptr<Face> Face::Finish(std::shared_ptr<Face> result) {
  Impl& impl = *result->impl_;
  // An empty face is what HarfBuzz gives for what is not a font.
  if (hb_face_get_glyph_count(impl.face) == 0) return nullptr;
  impl.upem = std::max(1u, hb_face_get_upem(impl.face));
  impl.font = hb_font_create(impl.face);
  hb_font_set_scale(impl.font, static_cast<int>(impl.upem), static_cast<int>(impl.upem));
  hb_ot_font_set_funcs(impl.font);

  Description& d = result->description_;
  d.legacyFamily = NameOf(impl.face, HB_OT_NAME_ID_FONT_FAMILY);
  d.family = NameOf(impl.face, HB_OT_NAME_ID_TYPOGRAPHIC_FAMILY);
  if (d.family.empty()) d.family = d.legacyFamily;
  d.fullName = NameOf(impl.face, HB_OT_NAME_ID_FULL_NAME);
  d.postScriptName = NameOf(impl.face, HB_OT_NAME_ID_POSTSCRIPT_NAME);
  d.weight = hb_style_get_value(impl.font, HB_STYLE_TAG_WEIGHT);
  d.width = hb_style_get_value(impl.font, HB_STYLE_TAG_WIDTH);
  d.italic = hb_style_get_value(impl.font, HB_STYLE_TAG_ITALIC) >= 0.5f;
  d.slant = -hb_style_get_value(impl.font, HB_STYLE_TAG_SLANT_ANGLE);
  if (d.weight <= 0) d.weight = 400;
  if (d.width <= 0) d.width = 100;
  d.weightMin = d.weightMax = d.weight;
  d.widthMin = d.widthMax = d.width;
  d.slantMin = d.slantMax = d.slant;
  unsigned axes = hb_ot_var_get_axis_count(impl.face);
  d.variable = axes > 0;
  if (axes > 0) {
    hb_ot_var_axis_info_t infos[16];
    unsigned count = 16;
    hb_ot_var_get_axis_infos(impl.face, 0, &count, infos);
    for (unsigned i = 0; i < count; ++i) {
      if (infos[i].tag == HB_TAG('w', 'g', 'h', 't')) { d.weightMin = infos[i].min_value; d.weightMax = infos[i].max_value; }
      else if (infos[i].tag == HB_TAG('w', 'd', 't', 'h')) { d.widthMin = infos[i].min_value; d.widthMax = infos[i].max_value; }
      else if (infos[i].tag == HB_TAG('s', 'l', 'n', 't')) { d.slantMin = -infos[i].max_value; d.slantMax = -infos[i].min_value; }
    }
  }

  result->ReadMetrics();
  return result;
}

void Face::ReadMetrics() {
  Impl& impl = *impl_;
  Metrics& m = metrics_;

  m.unitsPerEm = impl.upem;
  const double upem = impl.upem;
  hb_position_t value;
  // The metrics of the horizontal line: the typographic ones when the font says to use them, else hhea.
  if (hb_ot_metrics_get_position(impl.font, HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER, &value)) m.ascent = value / upem;
  if (hb_ot_metrics_get_position(impl.font, HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER, &value)) m.descent = -value / upem;
  if (hb_ot_metrics_get_position(impl.font, HB_OT_METRICS_TAG_HORIZONTAL_LINE_GAP, &value)) m.lineGap = value / upem;
  if (m.ascent + m.descent <= 0) {
    m.ascent = 0.8;
    m.descent = 0.2;
  }
  if (hb_ot_metrics_get_position(impl.font, HB_OT_METRICS_TAG_X_HEIGHT, &value) && value > 0) m.xHeight = value / upem;
  else {
    hb_codepoint_t glyph;
    hb_glyph_extents_t extents;
    if (hb_font_get_nominal_glyph(impl.font, 'x', &glyph) && hb_font_get_glyph_extents(impl.font, glyph, &extents)) m.xHeight = extents.y_bearing / upem;
  }
  if (hb_ot_metrics_get_position(impl.font, HB_OT_METRICS_TAG_CAP_HEIGHT, &value) && value > 0) m.capHeight = value / upem;
  else {
    hb_codepoint_t glyph;
    hb_glyph_extents_t extents;
    if (hb_font_get_nominal_glyph(impl.font, 'H', &glyph) && hb_font_get_glyph_extents(impl.font, glyph, &extents)) m.capHeight = extents.y_bearing / upem;
  }
  if (hb_ot_metrics_get_position(impl.font, HB_OT_METRICS_TAG_UNDERLINE_OFFSET, &value)) m.underlinePosition = value / upem;
  if (hb_ot_metrics_get_position(impl.font, HB_OT_METRICS_TAG_UNDERLINE_SIZE, &value)) m.underlineThickness = value / upem;
  if (hb_ot_metrics_get_position(impl.font, HB_OT_METRICS_TAG_STRIKEOUT_OFFSET, &value)) m.strikeoutPosition = value / upem;
  if (hb_ot_metrics_get_position(impl.font, HB_OT_METRICS_TAG_STRIKEOUT_SIZE, &value)) m.strikeoutThickness = value / upem;
}

bool Face::HasGlyph(char32_t codePoint) const {
  hb_codepoint_t glyph;
  return hb_font_get_nominal_glyph(impl_->font, codePoint, &glyph);
}

uint32_t Face::GlyphFor(char32_t codePoint) const {
  hb_codepoint_t glyph = 0;
  hb_font_get_nominal_glyph(impl_->font, codePoint, &glyph);
  return glyph;
}

double Face::Advance(char32_t codePoint) const {
  hb_codepoint_t glyph = 0;
  if (!hb_font_get_nominal_glyph(impl_->font, codePoint, &glyph)) glyph = 0;
  return hb_font_get_glyph_h_advance(impl_->font, glyph) / static_cast<double>(impl_->upem);
}

std::vector<Glyph> Face::Shape(std::string_view text, const ShapeOptions& options) const {
  hb_buffer_t* buffer = hb_buffer_create();
  hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
  hb_buffer_set_direction(buffer, options.rightToLeft ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
  if (!options.script.empty()) hb_buffer_set_script(buffer, hb_script_from_string(options.script.c_str(), static_cast<int>(options.script.size())));
  if (!options.language.empty()) hb_buffer_set_language(buffer, hb_language_from_string(options.language.c_str(), static_cast<int>(options.language.size())));
  hb_buffer_guess_segment_properties(buffer);
  std::vector<hb_feature_t> features;
  const auto add = [&](const char* tag, uint32_t value) {
    hb_feature_t feature;
    feature.tag = hb_tag_from_string(tag, 4);
    feature.value = value;
    feature.start = HB_FEATURE_GLOBAL_START;
    feature.end = HB_FEATURE_GLOBAL_END;
    features.push_back(feature);
  };
  if (!options.kerning) add("kern", 0);
  if (!options.ligatures) {
    add("liga", 0);
    add("clig", 0);
  }
  for (const ShapeOptions::Feature& f : options.features) {
    if (f.tag.size() == 4) add(f.tag.c_str(), f.value);
  }
  hb_shape(impl_->font, buffer, features.data(), static_cast<unsigned>(features.size()));
  unsigned count = 0;
  const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
  const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
  std::vector<Glyph> glyphs;
  glyphs.reserve(count);
  const double upem = impl_->upem;
  for (unsigned i = 0; i < count; ++i) {
    Glyph g;
    g.id = infos[i].codepoint;
    g.cluster = infos[i].cluster;
    g.advance = positions[i].x_advance / upem + options.letterSpacing;
    g.xOffset = positions[i].x_offset / upem;
    g.yOffset = positions[i].y_offset / upem;
    glyphs.push_back(g);
  }
  hb_buffer_destroy(buffer);
  return glyphs;
}

double Face::Measure(std::string_view text, const ShapeOptions& options) const {
  double width = 0;
  for (const Glyph& g : Shape(text, options)) width += g.advance;
  return width;
}

std::shared_ptr<Face> Face::WithVariations(const std::vector<Axis>& axes) const {
  if (!description_.variable || axes.empty()) return std::const_pointer_cast<Face>(shared_from_this());
  std::shared_ptr<Face> result(new Face());
  result->parent_ = shared_from_this();
  result->description_ = description_;
  result->impl_ = std::make_unique<Impl>();
  Impl& impl = *result->impl_;
  impl.blob = hb_blob_reference(impl_->blob);
  impl.face = hb_face_reference(impl_->face);
  impl.upem = impl_->upem;
  impl.font = hb_font_create(impl.face);
  hb_font_set_scale(impl.font, static_cast<int>(impl.upem), static_cast<int>(impl.upem));
  hb_ot_font_set_funcs(impl.font);
  std::vector<hb_variation_t> variations;
  for (const Axis& axis : axes) {
    if (axis.tag.size() != 4) continue;
    hb_variation_t v;
    v.tag = hb_tag_from_string(axis.tag.c_str(), 4);
    v.value = static_cast<float>(axis.value);
    variations.push_back(v);
  }
  hb_font_set_variations(impl.font, variations.data(), static_cast<unsigned>(variations.size()));
  result->ReadMetrics();
  return result;
}

std::string Face::Outline(uint32_t glyph) const {
  struct State {
    std::string out;
    double upem;
  } state;
  state.upem = impl_->upem;
  static hb_draw_funcs_t* funcs = [] {
    hb_draw_funcs_t* f = hb_draw_funcs_create();
    hb_draw_funcs_set_move_to_func(f, [](hb_draw_funcs_t*, void* data, hb_draw_state_t*, float x, float y, void*) {
      State* s = static_cast<State*>(data);
      s->out += "M " + std::to_string(x / s->upem) + " " + std::to_string(y / s->upem) + " ";
    }, nullptr, nullptr);
    hb_draw_funcs_set_line_to_func(f, [](hb_draw_funcs_t*, void* data, hb_draw_state_t*, float x, float y, void*) {
      State* s = static_cast<State*>(data);
      s->out += "L " + std::to_string(x / s->upem) + " " + std::to_string(y / s->upem) + " ";
    }, nullptr, nullptr);
    hb_draw_funcs_set_quadratic_to_func(f, [](hb_draw_funcs_t*, void* data, hb_draw_state_t*, float cx, float cy, float x, float y, void*) {
      State* s = static_cast<State*>(data);
      s->out += "Q " + std::to_string(cx / s->upem) + " " + std::to_string(cy / s->upem) + " " + std::to_string(x / s->upem) + " " + std::to_string(y / s->upem) + " ";
    }, nullptr, nullptr);
    hb_draw_funcs_set_cubic_to_func(f, [](hb_draw_funcs_t*, void* data, hb_draw_state_t*, float c1x, float c1y, float c2x, float c2y, float x, float y, void*) {
      State* s = static_cast<State*>(data);
      s->out += "C " + std::to_string(c1x / s->upem) + " " + std::to_string(c1y / s->upem) + " " + std::to_string(c2x / s->upem) + " " + std::to_string(c2y / s->upem) + " " +
                std::to_string(x / s->upem) + " " + std::to_string(y / s->upem) + " ";
    }, nullptr, nullptr);
    hb_draw_funcs_set_close_path_func(f, [](hb_draw_funcs_t*, void* data, hb_draw_state_t*, void*) { static_cast<State*>(data)->out += "Z "; }, nullptr, nullptr);
    hb_draw_funcs_make_immutable(f);
    return f;
  }();
  hb_font_draw_glyph(impl_->font, glyph, funcs, &state);
  return state.out;
}

}  // namespace solar::font
