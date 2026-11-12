#include "pdf_doc.h"

#include <R.h>
#include <Rinternals.h>
#include <R_ext/GraphicsEngine.h>

#include <systemfonts.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>

std::string pdf_num(double v) {
  char buf[32];
  if (v == (long long)v && std::abs(v) < 1e15) {
    std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
  } else {
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    // Trim trailing zeros and a trailing decimal point
    std::string s(buf);
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
      size_t last = s.find_last_not_of('0');
      if (last == dot) last--;
      s.erase(last + 1);
    }
    return s;
  }
  return buf;
}

std::string pdf_hex16(uint32_t gid) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%04X", gid & 0xFFFF);
  return buf;
}

std::string utf8_to_utf16be_hex(const std::string& utf8) {
  // Decode UTF-8 to codepoints, then emit UTF-16BE hex (with surrogates)
  std::string out;
  char tmp[8];
  size_t i = 0, n = utf8.size();
  while (i < n) {
    uint32_t cp;
    unsigned char c = utf8[i];
    if (c < 0x80) {
      cp = c;
      i += 1;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
      cp = ((c & 0x1F) << 6) | (utf8[i + 1] & 0x3F);
      i += 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
      cp = ((c & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) | (utf8[i + 2] & 0x3F);
      i += 3;
    } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
      cp = ((c & 0x07) << 18) | ((utf8[i + 1] & 0x3F) << 12) |
           ((utf8[i + 2] & 0x3F) << 6) | (utf8[i + 3] & 0x3F);
      i += 4;
    } else {
      i += 1;
      continue;
    }
    if (cp < 0x10000) {
      std::snprintf(tmp, sizeof(tmp), "%04X", cp);
      out += tmp;
    } else {
      cp -= 0x10000;
      std::snprintf(tmp, sizeof(tmp), "%04X", 0xD800 + (cp >> 10));
      out += tmp;
      std::snprintf(tmp, sizeof(tmp), "%04X", 0xDC00 + (cp & 0x3FF));
      out += tmp;
    }
  }
  return out;
}

static std::string flate_compress(const std::string& data) {
  uLongf bound = compressBound(data.size());
  std::string out(bound, '\0');
  if (compress2((Bytef*)&out[0], &bound, (const Bytef*)data.data(), data.size(),
                Z_BEST_COMPRESSION) != Z_OK) {
    return data;  // fall back to uncompressed (shouldn't happen)
  }
  out.resize(bound);
  return out;
}

// ---------------------------------------------------------------- PdfFont

PdfFont::PdfFont(const FontSettings2& font, int resource_id)
    : path(font.file), index(font.index), settings(font), subset_id(resource_id) {
  // Take ownership of the variation coordinates; the pointers in
  // FontSettings2 may reference transient storage owned by systemfonts
  if (font.n_axes > 0 && font.axes != nullptr && font.coords != nullptr) {
    axes_store.assign(font.axes, font.axes + font.n_axes);
    coords_store.assign(font.coords, font.coords + font.n_axes);
    settings.axes = axes_store.data();
    settings.coords = coords_store.data();
    settings.n_axes = font.n_axes;
  } else {
    settings.axes = nullptr;
    settings.coords = nullptr;
    settings.n_axes = 0;
  }
  resource_name = "F" + std::to_string(resource_id);
  if (!sfnt::parse(path, index, metrics)) {
    parse_failed = true;
  }
  // Subset glyph 0 is .notdef
  int adv0 = metrics.advances.empty() ? 0 : metrics.advances[0];
  sub.push_back({0, "", adv0});
}

uint32_t PdfFont::useGlyph(uint32_t orig_gid) {
  if (orig_gid == 0) return 0;
  auto it = gid_map.find(orig_gid);
  if (it != gid_map.end()) return it->second;

  std::string text;
  auto cp_it = metrics.cmap.find(orig_gid);
  if (cp_it != metrics.cmap.end() && cp_it->second != 0) {
    // Encode the codepoint as UTF-8
    uint32_t cp = cp_it->second;
    if (cp < 0x80) {
      text += (char)cp;
    } else if (cp < 0x800) {
      text += (char)(0xC0 | (cp >> 6));
      text += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      text += (char)(0xE0 | (cp >> 12));
      text += (char)(0x80 | ((cp >> 6) & 0x3F));
      text += (char)(0x80 | (cp & 0x3F));
    } else {
      text += (char)(0xF0 | (cp >> 18));
      text += (char)(0x80 | ((cp >> 12) & 0x3F));
      text += (char)(0x80 | ((cp >> 6) & 0x3F));
      text += (char)(0x80 | (cp & 0x3F));
    }
  }
  int adv = orig_gid < metrics.advances.size() ? metrics.advances[orig_gid] : 0;
  if (settings.n_axes > 0) {
    // hmtx carries default-instance advances; for an instanced font get the
    // advance at the variation coordinates instead. glyph_metrics is keyed
    // by codepoint, so map the glyph back through the cmap; glyphs without
    // a codepoint keep the hmtx value
    auto cp_it2 = metrics.cmap.find(orig_gid);
    if (cp_it2 != metrics.cmap.end() && cp_it2->second != 0) {
      double a, d, w;
      int upem = metrics.units_per_em;
      if (systemfonts::ver2::glyph_metrics(cp_it2->second, settings,
                                           (double)upem, 72.0, &a, &d,
                                           &w) == 0) {
        adv = (int)std::lround(w);
      }
    }
  }
  uint32_t new_gid = (uint32_t)sub.size();
  sub.push_back({orig_gid, text, adv});
  gid_map[orig_gid] = new_gid;
  return new_gid;
}

int PdfFont::writeObjects(PdfDoc& doc) {
  int upem = metrics.units_per_em;
  auto scale1000 = [&](int v) -> int {
    return (int)std::lround(double(v) * 1000.0 / upem);
  };

  // Extract outlines for the used glyphs and synthesize a subset TrueType
  // font from them (handles TrueType, CFF, and collection sources alike)
  size_t n_glyphs = sub.size();
  std::vector<sfnt::GlyfGlyph> outlines(n_glyphs);
  std::vector<uint16_t> advances(n_glyphs);
  double tol = 0.1 * upem / 1000.0;
  for (size_t i = 0; i < n_glyphs; i++) {
    advances[i] = (uint16_t)std::max(0, sub[i].advance);
    if (sub[i].orig_gid == 0) continue;
    bool no_outline = false;
    const std::string& path_str = glyphOutline(sub[i].orig_gid, &no_outline);
    if (!path_str.empty()) {
      outlines[i] = sfnt::parse_glyph_path(path_str, tol);
    }
  }
  std::vector<char> ttf =
      sfnt::build_ttf(outlines, advances, (uint16_t)upem, metrics.ascender,
                      metrics.descender, 0);

  // Embedded font file stream
  int fontfile_obj = doc.newObject();
  std::string font_data(ttf.begin(), ttf.end());
  doc.addStreamObject(fontfile_obj, "/Length1 " + std::to_string(font_data.size()),
                      font_data, true);

  // Subset tag: 6 uppercase letters derived from the font's id
  std::string tag(6, 'A');
  {
    unsigned int v = (unsigned int)subset_id;
    for (int i = 5; i >= 0; i--) {
      tag[i] = (char)('A' + v % 26);
      v /= 26;
    }
  }
  std::string base_name =
      metrics.postscript_name.empty() ? "EmbeddedFont" : metrics.postscript_name;
  std::string fontname = tag + "+" + base_name;

  // Font descriptor
  int fd_obj = doc.newObject();
  {
    std::ostringstream fd;
    fd << "<< /Type /FontDescriptor /FontName /" << fontname
       << " /Flags 4 /FontBBox [" << scale1000(metrics.bbox[0]) << " "
       << scale1000(metrics.bbox[1]) << " " << scale1000(metrics.bbox[2]) << " "
       << scale1000(metrics.bbox[3]) << "] /ItalicAngle "
       << pdf_num(metrics.italic_angle) << " /Ascent "
       << scale1000(metrics.ascender) << " /Descent "
       << scale1000(metrics.descender) << " /CapHeight "
       << scale1000(metrics.cap_height ? metrics.cap_height : metrics.ascender)
       << " /StemV 80 /FontFile2 " << fontfile_obj << " 0 R >>";
    doc.objects[fd_obj - 1] = fd.str();
  }

  // ToUnicode CMap
  int touni_obj = doc.newObject();
  {
    std::ostringstream cmap;
    cmap << "/CIDInit /ProcSet findresource begin\n12 dict begin\n"
            "begincmap\n/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) "
            "/Supplement 0 >> def\n"
            "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
            "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    size_t n = 0;
    for (size_t i = 0; i < n_glyphs; i++) {
      if (!sub[i].text.empty()) n++;
    }
    // bfchar blocks are limited to 100 entries by the spec
    size_t emitted = 0;
    size_t gi = 0;
    while (emitted < n) {
      size_t block = std::min(n - emitted, (size_t)100);
      cmap << block << " beginbfchar\n";
      for (size_t j = 0; j < block; gi++) {
        if (sub[gi].text.empty()) continue;
        cmap << "<" << pdf_hex16((uint32_t)gi) << "> <"
             << utf8_to_utf16be_hex(sub[gi].text) << ">\n";
        j++;
      }
      cmap << "endbfchar\n";
      emitted += block;
    }
    cmap << "endcmap\nCMapName currentdict /CMap defineresource "
            "pop\nend\nend\n";
    doc.addStreamObject(touni_obj, "", cmap.str(), false);
  }

  // Widths: subset gids are consecutive from 0
  std::ostringstream w;
  w << "[ 0 [";
  for (size_t i = 0; i < n_glyphs; i++) {
    w << " " << scale1000(advances[i]);
  }
  w << " ] ]";

  // CIDFont descendant
  int cid_obj = doc.newObject();
  {
    std::ostringstream cid;
    cid << "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /" << fontname
        << " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) "
           "/Supplement 0 >> /FontDescriptor "
        << fd_obj << " 0 R /DW 1000 /W " << w.str()
        << " /CIDToGIDMap /Identity >>";
    doc.objects[cid_obj - 1] = cid.str();
  }

  // Type0 wrapper
  int font_obj = doc.newObject();
  {
    std::ostringstream f;
    f << "<< /Type /Font /Subtype /Type0 /BaseFont /" << fontname
      << " /Encoding /Identity-H /DescendantFonts [" << cid_obj
      << " 0 R] /ToUnicode " << touni_obj << " 0 R >>";
    doc.objects[font_obj - 1] = f.str();
  }
  return font_obj;
}

// ----------------------------------------------------------------- PdfDoc

PdfDoc::PdfDoc(const std::string& file, double scaling)
    : filename(file), scale(scaling) {
  if (scale <= 0) scale = 1;
}

int PdfDoc::newObject() {
  objects.emplace_back();
  n_objects = (int)objects.size();
  return n_objects;
}

void PdfDoc::addStreamObject(int obj, const std::string& dict_extra,
                             const std::string& data, bool compress) {
  std::string payload = data;
  std::ostringstream s;
  s << "<< " << dict_extra;
  if (compress) {
    payload = flate_compress(data);
    s << " /Filter /FlateDecode";
  }
  s << " /Length " << payload.size() << " >>\nstream\n";
  s << payload << "\nendstream";
  objects[obj - 1] = s.str();
}

PdfFont* PdfDoc::getFont(const FontSettings2& font) {
  std::string key =
      std::string(font.file) + "#" + std::to_string(font.index);
  for (int i = 0; i < font.n_axes; i++) {
    key += "#" + std::to_string((uint32_t)font.axes[i]) + "=" +
           std::to_string(font.coords[i]);
  }
  auto it = fonts.find(key);
  if (it != fonts.end()) return it->second.get();
  auto new_font = std::unique_ptr<PdfFont>(new PdfFont(font, ++font_counter));
  PdfFont* ret = new_font.get();
  fonts.emplace(key, std::move(new_font));
  return ret;
}

const std::string& PdfFont::glyphOutline(uint32_t orig_gid, bool* no_outline) {
  auto it = outline_cache.find(orig_gid);
  if (it == outline_cache.end()) {
    it = outline_cache.emplace(orig_gid, fetchOutline(orig_gid)).first;
  }
  *no_outline = it->second.second;
  return it->second.first;
}

std::pair<std::string, bool> PdfFont::fetchOutline(uint32_t orig_gid) {
  double ident[6] = {1, 0, 0, 1, 0, 0};
  bool no_outline = false;
  std::string p = systemfonts::ver2::get_glyph_path(
      (int)orig_gid, ident, settings, (double)metrics.units_per_em,
      &no_outline);
  return {p, no_outline};
}

void PdfDoc::beginCapture() {
  stream_stack.push_back(std::move(content));
  content.str("");
  content.clear();
}

std::string PdfDoc::endCapture() {
  std::string s = content.str();
  content = std::move(stream_stack.back());
  stream_stack.pop_back();
  return s;
}

std::string PdfDoc::addForm(const std::string& stream, bool gray_group,
                            bool isolated) {
  int obj = newObject();
  std::ostringstream dict;
  // BBox is in form space, which is device units (points * scaling)
  dict << "/Type /XObject /Subtype /Form /BBox [0 0 "
       << pdf_num(page_width_pts * scale) << " "
       << pdf_num(page_height_pts * scale)
       << "] /Group << /S /Transparency /CS " << (gray_group ? "/DeviceGray" : "/DeviceRGB")
       << (isolated ? " /I true" : "")
       << " >> /Resources @RESOURCES@";
  addStreamObject(obj, dict.str(), stream, true);
  std::string name = "Fm" + std::to_string(++form_counter);
  form_objs.emplace_back(name, obj);
  return name;
}

int PdfDoc::addStitchingFunction(const std::vector<double>& stops_in,
                                 const std::vector<unsigned int>& cols_in,
                                 bool gray) {
  size_t n = stops_in.size();
  // A degenerate gradient collapses to a flat fill at its single color
  if (n == 1) return addStitchingFunction({0.0, 1.0}, {cols_in[0], cols_in[0]},
                                          gray);

  // grid permits unsorted and duplicate stops; the stitching function
  // requires sorted stops with strictly increasing interior bounds
  std::vector<size_t> order(n);
  for (size_t i = 0; i < n; i++) order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) { return stops_in[a] < stops_in[b]; });
  std::vector<double> stops(n);
  std::vector<unsigned int> cols(n);
  for (size_t i = 0; i < n; i++) {
    stops[i] = stops_in[order[i]];
    cols[i] = cols_in[order[i]];
  }
  for (size_t i = 1; i + 1 < n; i++) {
    if (stops[i] <= stops[i - 1]) stops[i] = std::nextafter(stops[i - 1], 1.0);
  }

  std::vector<int> fn_objs;
  for (size_t i = 0; i + 1 < n; i++) {
    int obj = newObject();
    std::ostringstream f;
    auto comp = [&](int j, bool g, std::ostringstream& o) {
      if (g) {
        double lum = (0.2126 * R_RED(cols[j]) + 0.7152 * R_GREEN(cols[j]) +
                      0.0722 * R_BLUE(cols[j])) / 255.0;
        o << "[" << pdf_num(lum) << "]";
      } else {
        o << "[" << pdf_num(R_RED(cols[j]) / 255.0) << " "
          << pdf_num(R_GREEN(cols[j]) / 255.0) << " "
          << pdf_num(R_BLUE(cols[j]) / 255.0) << "]";
      }
    };
    f << "<< /FunctionType 2 /Domain [0 1] /C0 ";
    comp(i, gray, f);
    f << " /C1 ";
    comp(i + 1, gray, f);
    f << " /N 1 >>";
    objects[obj - 1] = f.str();
    fn_objs.push_back(obj);
  }
  int obj = newObject();
  std::ostringstream f;
  if (fn_objs.size() == 1) {
    // Two stops only: single exponential interpolation
    objects[obj - 1] = objects[fn_objs[0] - 1];
    return obj;
  }
  f << "<< /FunctionType 3 /Domain [0 1] /Functions [";
  for (int o : fn_objs) f << " " << o << " 0 R";
  f << " ] /Bounds [";
  for (size_t i = 1; i + 1 < n; i++) f << " " << pdf_num(stops[i]);
  f << " ] /Encode [";
  for (size_t i = 0; i + 1 < n; i++) f << " 0 1";
  f << " ] >>";
  objects[obj - 1] = f.str();
  return obj;
}

int PdfDoc::addShading(bool radial, const double coords[6],
                       const std::vector<double>& stops,
                       const std::vector<unsigned int>& cols, int extend,
                       bool gray) {
  int fn = addStitchingFunction(stops, cols, gray);
  int obj = newObject();
  std::ostringstream s;
  s << "<< /ShadingType " << (radial ? 3 : 2)
    << " /ColorSpace " << (gray ? "/DeviceGray" : "/DeviceRGB") << " /Coords [";
  int ncoords = radial ? 6 : 4;
  for (int i = 0; i < ncoords; i++) s << " " << pdf_num(coords[i]);
  s << " ] /Function " << fn << " 0 R /Extend ["
    << (extend == 1 ? "true" : "false") << " "
    << (extend == 1 ? "true" : "false") << "] >>";
  objects[obj - 1] = s.str();
  return obj;
}

std::string PdfDoc::addAxialPattern(double x1, double y1, double x2, double y2,
                                    const std::vector<double>& stops,
                                    const std::vector<unsigned int>& cols,
                                    int extend, std::string* alpha_gs) {
  double coords[6] = {x1, y1, x2, y2, 0, 0};
  return addGradientPattern(false, coords, stops, cols, extend, alpha_gs);
}

std::string PdfDoc::addRadialPattern(double cx1, double cy1, double r1,
                                     double cx2, double cy2, double r2,
                                     const std::vector<double>& stops,
                                     const std::vector<unsigned int>& cols,
                                     int extend, std::string* alpha_gs) {
  double coords[6] = {cx1, cy1, r1, cx2, cy2, r2};
  return addGradientPattern(true, coords, stops, cols, extend, alpha_gs);
}

std::string PdfDoc::addGradientPattern(bool radial, const double coords[6],
                                       const std::vector<double>& stops,
                                       const std::vector<unsigned int>& cols,
                                       int extend, std::string* alpha_gs) {
  int sh = addShading(radial, coords, stops, cols, extend, false);
  int obj = newObject();
  std::ostringstream p;
  p << "<< /Type /Pattern /PatternType 2 /Shading " << sh << " 0 R >>";
  objects[obj - 1] = p.str();
  std::string name = "P" + std::to_string(++pattern_counter);
  pattern_objs.emplace_back(name, obj);

  alpha_gs->clear();
  bool has_alpha = false;
  for (unsigned int c : cols) {
    if (R_ALPHA(c) != 255) has_alpha = true;
  }
  if (has_alpha) {
    // Alpha gradient: same geometry with gray = alpha, painted into a form
    // used as a luminosity soft mask
    std::vector<unsigned int> alpha_cols(cols.size());
    for (size_t i = 0; i < cols.size(); i++) {
      unsigned int a = R_ALPHA(cols[i]);
      alpha_cols[i] = R_RGB(a, a, a);
    }
    int sh_gray = addShading(radial, coords, stops, alpha_cols, extend, true);
    std::string sh_name = "Sh" + std::to_string(sh_gray);
    shading_objs.emplace_back(sh_name, sh_gray);
    std::ostringstream fs;
    fs << "q\n/" << sh_name << " sh\nQ\n";
    std::string form = addForm(fs.str(), true);
    *alpha_gs = addGState(1.0, 1.0, "", form, true);
  }
  return name;
}

std::string PdfDoc::addTilingPattern(double x, double y, double width,
                                     double height,
                                     const std::string& tile_stream,
                                     int extend) {
  double xstep = width, ystep = height;
  if (extend == 4 || extend == 1) {
    // "none" and "pad": approximate with a single tile
    xstep = std::max(xstep, page_width_pts * scale * 2);
    ystep = std::max(ystep, page_height_pts * scale * 2);
  }
  int obj = newObject();
  std::ostringstream dict;
  dict << "/Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 /BBox ["
       << pdf_num(x) << " " << pdf_num(y) << " " << pdf_num(x + width) << " "
       << pdf_num(y + height) << "] /XStep " << pdf_num(xstep) << " /YStep "
       << pdf_num(ystep) << " /Resources @RESOURCES@";
  addStreamObject(obj, dict.str(), tile_stream, true);
  std::string name = "P" + std::to_string(++pattern_counter);
  pattern_objs.emplace_back(name, obj);
  return name;
}

void PdfDoc::setInfo(const std::string& title, const std::string& author,
                     const std::string& creator, const std::string& subject,
                     const std::string& keywords,
                     const std::string& creation_date) {
  info_title = title;
  info_author = author;
  info_creator = creator;
  info_subject = subject;
  info_keywords = keywords;
  info_creation_date = creation_date;
}

// PNG predictor (optimum, /Predictor 15): pick the per-row filter (None,
// Sub, or Up) that minimises the sum of absolute filtered bytes, then
// prefix each row with its filter byte
static std::string png_predict_encode(const std::string& data, int row_bytes,
                                      int height, int bpp) {
  std::string out;
  out.reserve(data.size() + height);
  std::string sub(row_bytes, '\0'), up(row_bytes, '\0');
  for (int y = 0; y < height; y++) {
    const unsigned char* cur =
        (const unsigned char*)data.data() + size_t(y) * row_bytes;
    const unsigned char* prev = y > 0 ? cur - row_bytes : nullptr;
    long score_none = 0, score_sub = 0, score_up = 0;
    for (int x = 0; x < row_bytes; x++) {
      unsigned char left = x >= bpp ? cur[x - bpp] : 0;
      unsigned char above = prev ? prev[x] : 0;
      sub[x] = (unsigned char)(cur[x] - left);
      up[x] = (unsigned char)(cur[x] - above);
      score_none += std::abs((signed char)cur[x]);
      score_sub += std::abs((signed char)sub[x]);
      score_up += std::abs((signed char)up[x]);
    }
    if (score_none <= score_sub && score_none <= score_up) {
      out += '\0';
      out.append((const char*)cur, row_bytes);
    } else if (score_sub <= score_up) {
      out += '\1';
      out += sub;
    } else {
      out += '\2';
      out += up;
    }
  }
  return out;
}

std::string PdfDoc::addImage(int width, int height, const unsigned char* rgba,
                             bool interpolate, ImageColor color) {
  size_t n = size_t(width) * height;

  // Decide the colour plane: forced conversion, or RGB with a fall-back to
  // gray when no pixel carries chroma
  int channels = 3;
  const char* colorspace = "/DeviceRGB";
  if (color == ImageColor::kGray) {
    channels = 1;
    colorspace = "/DeviceGray";
  } else if (color == ImageColor::kCMYK) {
    channels = 4;
    colorspace = "/DeviceCMYK";
  } else {
    bool all_gray = true;
    for (size_t i = 0; i < n; i++) {
      if (rgba[i * 4] != rgba[i * 4 + 1] || rgba[i * 4] != rgba[i * 4 + 2]) {
        all_gray = false;
        break;
      }
    }
    if (all_gray) {
      channels = 1;
      colorspace = "/DeviceGray";
    }
  }

  // Split into colour and alpha planes, converting the colour as needed
  std::string plane, alpha;
  plane.reserve(n * channels);
  alpha.reserve(n);
  bool has_alpha = false;
  for (size_t i = 0; i < n; i++) {
    double r = rgba[i * 4 + 0] / 255.0, g = rgba[i * 4 + 1] / 255.0,
           b = rgba[i * 4 + 2] / 255.0;
    switch (channels) {
      case 1:
        if (color == ImageColor::kAuto) {
          plane += rgba[i * 4];
        } else {
          plane += (char)std::lround(
              255.0 * (0.2126 * r + 0.7152 * g + 0.0722 * b));
        }
        break;
      case 4: {
        // Same naive RGB->CMYK as the vector colour operators
        double k = 1.0 - std::max(r, std::max(g, b));
        double d = 1.0 - k;
        double c = d > 0 ? (d - r) / d : 0.0;
        double m = d > 0 ? (d - g) / d : 0.0;
        double y = d > 0 ? (d - b) / d : 0.0;
        plane += (char)std::lround(255.0 * c);
        plane += (char)std::lround(255.0 * m);
        plane += (char)std::lround(255.0 * y);
        plane += (char)std::lround(255.0 * k);
        break;
      }
      default:
        plane += rgba[i * 4 + 0];
        plane += rgba[i * 4 + 1];
        plane += rgba[i * 4 + 2];
    }
    alpha += rgba[i * 4 + 3];
    if (rgba[i * 4 + 3] != 255) has_alpha = true;
  }

  int smask_obj = 0;
  if (has_alpha) {
    smask_obj = newObject();
    std::ostringstream dict;
    dict << "/Type /XObject /Subtype /Image /Width " << width << " /Height "
         << height << " /ColorSpace /DeviceGray /BitsPerComponent 8"
         << " /DecodeParms << /Predictor 15 /Colors 1 /BitsPerComponent 8"
         << " /Columns " << width << " >>";
    addStreamObject(smask_obj, dict.str(),
                    png_predict_encode(alpha, width, height, 1), true);
  }

  int img_obj = newObject();
  std::ostringstream dict;
  dict << "/Type /XObject /Subtype /Image /Width " << width << " /Height "
       << height << " /ColorSpace " << colorspace << " /BitsPerComponent 8"
       << " /DecodeParms << /Predictor 15 /Colors " << channels
       << " /BitsPerComponent 8 /Columns " << width << " >>";
  if (interpolate) dict << " /Interpolate true";
  if (smask_obj) dict << " /SMask " << smask_obj << " 0 R";
  addStreamObject(img_obj, dict.str(),
                  png_predict_encode(plane, width * channels, height,
                                     channels),
                  true);

  std::string name = "Im" + std::to_string(++image_counter);
  image_objs.emplace_back(name, img_obj);
  return name;
}

std::string PdfDoc::addGState(double stroke_alpha, double fill_alpha,
                              const std::string& blend,
                              const std::string& smask_form,
                              bool smask_luminosity, bool clear_smask,
                              bool invert_smask) {
  std::ostringstream key;
  key << std::lround(stroke_alpha * 1000.0) << ","
      << std::lround(fill_alpha * 1000.0) << "," << blend << "," << smask_form
      << "," << smask_luminosity << "," << clear_smask << "," << invert_smask;
  auto it = gstate_names.find(key.str());
  if (it != gstate_names.end()) return it->second;

  std::string name = "GS" + std::to_string(++gstate_counter);
  int obj = newObject();
  std::ostringstream s;
  s << "<< /Type /ExtGState /CA " << pdf_num(stroke_alpha) << " /ca "
    << pdf_num(fill_alpha);
  if (!blend.empty()) s << " /BM /" << blend;
  if (!smask_form.empty()) {
    int form_obj = -1;
    for (auto& kv : form_objs) {
      if (kv.first == smask_form) form_obj = kv.second;
    }
    if (form_obj > 0) {
      s << " /SMask << /S /" << (smask_luminosity ? "Luminosity" : "Alpha")
        << " /G " << form_obj << " 0 R";
      if (invert_smask) {
        // Transfer function x -> 1 - x: yields the inverse mask ("out")
        s << " /TR << /FunctionType 2 /Domain [0 1] /C0 [1] /C1 [0] /N 1 >>";
      }
      s << " >>";
    }
  } else if (clear_smask) {
    s << " /SMask /None";
  }
  s << " >>";
  objects[obj - 1] = s.str();
  gstate_names[key.str()] = name;
  gstate_objs.emplace_back(name, obj);
  return name;
}

void PdfDoc::beginPage(double width_in, double height_in) {
  Page p;
  p.width = width_in * 72.0;
  p.height = height_in * 72.0;
  page_width_pts = p.width;
  page_height_pts = p.height;
  pages.push_back(p);
  content.str("");
  content.clear();
  page_open = true;

  // Map device units (points * scaling) to physical points
  content << pdf_num(1.0 / scale) << " 0 0 " << pdf_num(1.0 / scale)
          << " 0 0 cm\nq\n";
}

void PdfDoc::endPage() {
  if (!page_open) return;
  Page& p = pages.back();
  content << "Q\n";
  p.content = content.str();
  page_open = false;
}

std::string PdfDoc::resourcesString(
    const std::map<std::string, int>& font_objs) const {
  std::ostringstream res;
  res << "<< /Font <<";
  for (auto& kv : font_objs) res << " /" << kv.first << " " << kv.second << " 0 R";
  res << " >> /XObject <<";
  for (auto& kv : image_objs) res << " /" << kv.first << " " << kv.second << " 0 R";
  for (auto& kv : form_objs) res << " /" << kv.first << " " << kv.second << " 0 R";
  res << " >> /ExtGState <<";
  for (auto& kv : gstate_objs) res << " /" << kv.first << " " << kv.second << " 0 R";
  res << " >> /Pattern <<";
  for (auto& kv : pattern_objs) res << " /" << kv.first << " " << kv.second << " 0 R";
  res << " >> /Shading <<";
  for (auto& kv : shading_objs) res << " /" << kv.first << " " << kv.second << " 0 R";
  res << " >> >>";
  return res.str();
}

bool PdfDoc::finish() {
  // Write all font objects and remember resource names -> object numbers
  std::map<std::string, int> font_objs;
  for (auto& kv : fonts) {
    PdfFont* f = kv.second.get();
    if (f->parse_failed || f->sub.size() <= 1) continue;
    font_objs[f->resource_name] = f->writeObjects(*this);
  }

  std::string resources = resourcesString(font_objs);

  // Patch the resource placeholder in forms and tiling patterns
  for (auto& body : objects) {
    size_t pos;
    while ((pos = body.find("@RESOURCES@")) != std::string::npos) {
      body.replace(pos, 11, resources);
    }
  }

  // Pages
  std::vector<int> page_objs;
  for (auto& p : pages) {
    int contents_obj = newObject();
    addStreamObject(contents_obj, "", p.content, true);

    int page_obj = newObject();
    std::ostringstream po;
    po << "<< /Type /Page /Parent 0 0 R /MediaBox [0 0 " << pdf_num(p.width)
       << " " << pdf_num(p.height) << "] /Resources " << resources
       << " /Contents " << contents_obj << " 0 R >>";
    objects[page_obj - 1] = po.str();
    page_objs.push_back(page_obj);
  }

  int pages_obj = newObject();
  int catalog_obj = newObject();
  {
    std::ostringstream s;
    s << "<< /Type /Pages /Count " << page_objs.size() << " /Kids [";
    for (int o : page_objs) s << " " << o << " 0 R";
    s << " ] >>";
    objects[pages_obj - 1] = s.str();
    // Fix parent references now that the pages object number is known
    for (int o : page_objs) {
      std::string& body = objects[o - 1];
      size_t pos = body.find("/Parent 0 0 R");
      if (pos != std::string::npos) {
        body.replace(pos, 13, "/Parent " + std::to_string(pages_obj) + " 0 R");
      }
    }
    objects[catalog_obj - 1] =
        "<< /Type /Catalog /Pages " + std::to_string(pages_obj) + " 0 R >>";
  }

  // Document info dictionary
  int info_obj = newObject();
  {
    std::ostringstream info;
    auto pdf_date = []() {
      char buf[32];
      std::time_t t = std::time(nullptr);
      std::tm* tm = std::localtime(&t);
      // strftime over snprintf: gcc's -Wformat-truncation cannot prove the
      // tm fields are small enough for fixed-width %04d formats
      std::strftime(buf, sizeof(buf), "D:%Y%m%d%H%M%S", tm);
      return std::string(buf);
    };
    auto text_str = [](const std::string& utf8) -> std::string {
      // ASCII strings as literals; anything else as UTF-16BE hex with BOM
      bool ascii = true;
      for (unsigned char c : utf8) {
        if (c < 32 || c > 126) ascii = false;
      }
      if (ascii) {
        // Literal strings need parens and backslashes escaped
        std::string esc;
        esc.reserve(utf8.size());
        for (char c : utf8) {
          if (c == '(' || c == ')' || c == '\\') esc += '\\';
          esc += c;
        }
        return "(" + esc + ")";
      }
      return "<FEFF" + utf8_to_utf16be_hex(utf8) + ">";
    };
    info << "<< /Producer (portable R package)";
    if (!info_title.empty()) info << " /Title " << text_str(info_title);
    if (!info_author.empty()) info << " /Author " << text_str(info_author);
    if (!info_creator.empty()) info << " /Creator " << text_str(info_creator);
    if (!info_subject.empty()) info << " /Subject " << text_str(info_subject);
    if (!info_keywords.empty()) info << " /Keywords " << text_str(info_keywords);
    info << " /CreationDate ("
         << (info_creation_date.empty() ? pdf_date() : info_creation_date)
         << ") >>";
    objects[info_obj - 1] = info.str();
  }

  std::ofstream out(filename, std::ios::binary);
  if (!out) return false;
  out << "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
  std::vector<long> offsets(objects.size() + 1, 0);
  for (size_t i = 0; i < objects.size(); i++) {
    offsets[i + 1] = out.tellp();
    out << (i + 1) << " 0 obj\n" << objects[i] << "\nendobj\n";
  }
  long xref = out.tellp();
  out << "xref\n0 " << objects.size() + 1 << "\n"
      << "0000000000 65535 f \n";
  for (size_t i = 1; i <= objects.size(); i++) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%010ld 00000 n \n", offsets[i]);
    out << buf;
  }
  out << "trailer\n<< /Size " << objects.size() + 1 << " /Root " << catalog_obj
      << " 0 R /Info " << info_obj << " 0 R >>\nstartxref\n" << xref
      << "\n%%EOF\n";
  return (bool)out;
}
