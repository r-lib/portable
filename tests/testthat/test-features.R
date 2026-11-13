# Tests for the modern graphics engine features: patterns/gradients,
# clipping paths, masks, compositing groups, glyphs, and metadata.

render_grid <- function(code, ...) {
  file <- tempfile(fileext = ".pdf")
  portable_pdf(file, ...)
  grid::grid.newpage()
  force(code)
  dev.off()
  file
}

raw_pdf <- function(file) readBin(file, "raw", file.size(file))

test_that("dev.capabilities advertises the supported features", {
  skip_if(getRversion() < "4.2.0")  # dev.capabilities() was added in R 4.2.0
  portable_pdf(tempfile(fileext = ".pdf"))
  on.exit(dev.off())
  cap <- dev.capabilities()
  expect_true(is.character(cap$patterns))
  expect_true(all(c("LinearGradient", "RadialGradient", "TilingPattern") %in%
                  cap$patterns))
  expect_true(cap$clippingPaths)
  expect_equal(cap$masks, c("alpha", "luminance"))
  expect_true(all(c("over", "multiply", "screen") %in% cap$compositing))
  expect_true(cap$transformations)
  expect_true(cap$paths)
  expect_true(cap$semiTransparency)
})

test_that("linear gradients become axial shading patterns", {
  skip_if(getRversion() < "4.2.0")
  file <- render_grid(
    grid::grid.rect(gp = grid::gpar(
      fill = grid::linearGradient(c("red", "blue"))))
  )
  raw <- raw_pdf(file)
  expect_gt(grepRaw("/PatternType 2", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("/ShadingType 2", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("/FunctionType 2", raw, fixed = TRUE), 0)
  content <- paste(pdf_streams(file), collapse = "\n")
  expect_match(content, "/Pattern cs /P1 scn", fixed = TRUE)
})

test_that("radial gradients become radial shading patterns", {
  skip_if(getRversion() < "4.2.0")
  file <- render_grid(
    grid::grid.circle(gp = grid::gpar(
      fill = grid::radialGradient(c("yellow", "darkgreen"))))
  )
  raw <- raw_pdf(file)
  expect_gt(grepRaw("/ShadingType 3", raw, fixed = TRUE), 0)
})

test_that("gradients with alpha get a luminosity soft mask", {
  skip_if(getRversion() < "4.2.0")
  file <- render_grid(
    grid::grid.rect(gp = grid::gpar(
      fill = grid::linearGradient(c("black", "transparent"))))
  )
  raw <- raw_pdf(file)
  expect_gt(grepRaw("/S /Luminosity", raw, fixed = TRUE), 0)
})

test_that("multi-stop gradients use stitching functions", {
  skip_if(getRversion() < "4.2.0")
  file <- render_grid(
    grid::grid.rect(gp = grid::gpar(
      fill = grid::linearGradient(c("red", "white", "blue"))))
  )
  raw <- raw_pdf(file)
  expect_gt(grepRaw("/FunctionType 3", raw, fixed = TRUE), 0)
})

test_that("unsorted and duplicate gradient stops are handled", {
  skip_if(getRversion() < "4.2.0")
  file <- render_grid(
    grid::grid.rect(gp = grid::gpar(
      fill = grid::linearGradient(c("red", "white", "blue", "green"),
                                  stops = c(0, 0.8, 0.3, 0.3))))
  )
  raw <- raw_pdf(file)
  # Interior bounds must be sorted and increasing after normalization
  expect_gt(grepRaw("/Bounds [ 0.3 0.3 ]", raw, fixed = TRUE), 0)
})

test_that("tiling patterns become type 1 patterns", {
  skip_if(getRversion() < "4.2.0")
  file <- render_grid(
    grid::grid.rect(gp = grid::gpar(
      fill = grid::pattern(
        grid::circleGrob(r = 0.02, gp = grid::gpar(fill = "black")),
        width = grid::unit(5, "mm"), height = grid::unit(5, "mm"),
        extend = "repeat")))
  )
  raw <- raw_pdf(file)
  expect_gt(grepRaw("/PatternType 1", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("/TilingType 1", raw, fixed = TRUE), 0)
  # 5mm = 14.17pt tile step
  expect_gt(grepRaw("/XStep 14.17", raw, fixed = TRUE), 0)
})

test_that("clipping paths clip the output", {
  skip_if(getRversion() < "4.2.0")
  file <- render_grid({
    grid::pushViewport(grid::viewport(clip = grid::circleGrob(r = 0.3)))
    grid::grid.rect(gp = grid::gpar(fill = "grey90"))
    grid::popViewport()
  })
  content <- paste(pdf_streams(file), collapse = "\n")
  # Bezier circle path followed by a clip operator
  expect_match(content, "c\nW n", fixed = TRUE)
})

test_that("alpha masks become soft mask gstates", {
  skip_if(getRversion() < "4.2.0")
  file <- render_grid(
    grid::grid.rect(
      gp = grid::gpar(fill = "blue"),
      vp = grid::viewport(mask = grid::as.mask(grid::rectGrob(
        gp = grid::gpar(fill = "black")))))
  )
  raw <- raw_pdf(file)
  expect_gt(grepRaw("/SMask << /S /Alpha", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("/Group << /S /Transparency", raw, fixed = TRUE), 0)
})

test_that("luminance masks become luminosity soft masks", {
  skip_if(getRversion() < "4.2.0")
  skip_if_not(exists("as.mask", envir = asNamespace("grid")))
  mask <- grid::as.mask(grid::rectGrob(gp = grid::gpar(fill = "black")),
                        type = "luminance")
  file <- render_grid(
    grid::grid.rect(gp = grid::gpar(fill = "blue"),
                    vp = grid::viewport(mask = mask)))
  raw <- raw_pdf(file)
  expect_gt(grepRaw("/SMask << /S /Luminosity", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("/Group << /S /Transparency", raw, fixed = TRUE), 0)
})

test_that("groups become transparency group form XObjects with blend modes", {
  skip_if(getRversion() < "4.2.0")
  file <- render_grid(
    grid::grid.group(
      grid::circleGrob(x = 0.4, r = 0.2,
                       gp = grid::gpar(fill = "red", col = NA)),
      op = "multiply",
      dst = grid::circleGrob(x = 0.6, r = 0.2,
                             gp = grid::gpar(fill = "green", col = NA)))
  )
  raw <- raw_pdf(file)
  expect_gt(grepRaw("/Subtype /Form", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("/BM /Multiply", raw, fixed = TRUE), 0)
  content <- paste(pdf_streams(file), collapse = "\n")
  expect_match(content, " Do\n", fixed = TRUE)
})

test_that("porter-duff operators use alpha soft masks", {
  skip_if(getRversion() < "4.2.0")
  file <- render_grid(
    grid::grid.group(
      grid::circleGrob(x = 0.4, r = 0.2,
                       gp = grid::gpar(fill = "red", col = NA)),
      op = "xor",
      dst = grid::circleGrob(x = 0.6, r = 0.2,
                             gp = grid::gpar(fill = "green", col = NA)))
  )
  raw <- raw_pdf(file)
  # Two inverted alpha masks for the xor recipe
  expect_gt(grepRaw("/S /Alpha", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("/C0 [1] /C1 [0]", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("/I true", raw, fixed = TRUE), 0)
})

test_that("non-synthesizable compositing operators warn", {
  skip_if(getRversion() < "4.2.0")
  file <- tempfile(fileext = ".pdf")
  portable_pdf(file)
  grid::grid.newpage()
  expect_warning(
    grid::grid.group(grid::circleGrob(r = 0.2), op = "add",
                     dst = grid::circleGrob(x = 0.6, r = 0.2)),
    "not expressible"
  )
  dev.off()
})

test_that("grid.path strokes and fills work", {
  skip_if(getRversion() < "4.2.0")
  file <- render_grid(
    grid::grid.path(c(0.2, 0.8, 0.5), c(0.2, 0.2, 0.8),
                    gp = grid::gpar(fill = "grey80", col = "black"))
  )
  content <- paste(pdf_streams(file), collapse = "\n")
  expect_match(content, " m\n", fixed = TRUE)
  expect_match(content, "B\n", fixed = TRUE)
})

test_that("fill rules are honoured", {
  skip_if(getRversion() < "4.2.0")
  # Concentric squares in the same orientation: even-odd makes a hole,
  # nonzero winding fills everything
  holey_path <- function(rule) {
    grid::grid.draw(grid::pathGrob(
      x = c(0.2, 0.8, 0.8, 0.2, 0.35, 0.65, 0.65, 0.35),
      y = c(0.2, 0.2, 0.8, 0.8, 0.35, 0.35, 0.65, 0.65),
      id.lengths = c(4, 4), rule = rule, gp = grid::gpar(fill = "red")
    ))
  }
  evenodd <- paste(pdf_streams(render_grid(holey_path("evenodd"))),
                   collapse = "\n")
  winding <- paste(pdf_streams(render_grid(holey_path("winding"))),
                   collapse = "\n")
  expect_match(evenodd, "B*\n", fixed = TRUE)
  expect_match(winding, "B\n", fixed = TRUE)
  expect_false(grepl("B*", winding, fixed = TRUE))

  # The plain polygon primitive always uses nonzero winding (holes are
  # expressed by reversed subpath orientation)
  poly <- paste(pdf_streams(render_grid(grid::grid.polygon(
    x = c(0.2, 0.8, 0.8, 0.2, 0.35, 0.35, 0.65, 0.65),
    y = c(0.2, 0.2, 0.8, 0.8, 0.35, 0.65, 0.65, 0.35),
    id = rep(1:2, each = 4), gp = grid::gpar(fill = "red")
  ))), collapse = "\n")
  expect_match(poly, "B\n", fixed = TRUE)
  expect_false(grepl("B*", poly, fixed = TRUE))
})

test_that("document metadata is written to the Info dictionary", {
  file <- render_pdf(
    plot(1:10),
    metadata = pdf_metadata(title = "My title", author = "Jane",
                            subject = "A plot", keywords = c("a", "b"),
                            creation_date = .POSIXct(0, tz = "UTC"))
  )
  raw <- raw_pdf(file)
  expect_gt(grepRaw("/Title (My title)", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("/Author (Jane)", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("/Subject (A plot)", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("/Keywords (a, b)", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("/CreationDate (D:19700101000000Z)", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("/Producer", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("/Info ", raw, fixed = TRUE), 0)
})

test_that("special characters in metadata are escaped", {
  file <- render_pdf(plot(1:10),
                     metadata = pdf_metadata(title = "a (b) \\ c"))
  raw <- raw_pdf(file)
  expect_gt(grepRaw("/Title (a \\(b\\) \\\\ c)", raw, fixed = TRUE), 0)
})

test_that("pdf_metadata validates its inputs", {
  expect_s3_class(pdf_metadata(), "portable_pdf_metadata")
  expect_error(pdf_metadata(creation_date = "yesterday"), "POSIXct")
  expect_error(pdf_metadata(title = c("a", "b")))
})

test_that("ToUnicode CMaps with many glyphs use chunked bfchar blocks", {
  file <- render_pdf({
    plot.new()
    # More than 100 distinct glyphs
    text(0.5, 0.5, intToUtf8(33:200), cex = 0.5)
  })
  raw <- raw_pdf(file)
  # No block may exceed the 100-entry limit; multiple blocks are emitted
  blocks <- grepRaw("beginbfchar", raw, all = TRUE, fixed = TRUE)
  expect_gt(length(blocks), 1)
  expect_gt(grepRaw("100 beginbfchar", raw, fixed = TRUE), 0)
})

test_that("colormodel gray and cmyk affect color operators", {
  f_gray <- render_pdf(plot(1:10, col = "red"), colormodel = "gray")
  streams <- paste(pdf_streams(f_gray), collapse = "\n")
  expect_false(grepl(" RG", streams, fixed = FALSE))
  expect_match(streams, " G\n", fixed = TRUE)

  f_cmyk <- render_pdf(plot(1:10, col = "red"), colormodel = "cmyk")
  streams <- paste(pdf_streams(f_cmyk), collapse = "\n")
  expect_match(streams, " K\n", fixed = TRUE)
  # red in CMYK: c=0 m=1 y=1 k=0
  expect_match(streams, "0 1 1 0 K", fixed = TRUE)
})

test_that("stroke and fill with different alphas use separate passes", {
  file <- render_pdf({
    plot.new()
    plot.window(c(0, 1), c(0, 1))
    rect(0.2, 0.2, 0.8, 0.8,
         col = grDevices::adjustcolor("blue", 0.3),
         border = grDevices::adjustcolor("red", 0.8))
  })
  content <- paste(pdf_streams(file), collapse = "\n")
  # fill alpha and stroke alpha differ -> two paint passes of the rect
  expect_equal(lengths(regmatches(content, gregexpr(" re\n", content,
                                                    fixed = TRUE)))[[1]], 2)
  # the gstate dictionaries live in the object bodies, not the streams
  raw <- raw_pdf(file)
  expect_gt(grepRaw("/ca 0.3", raw, fixed = TRUE), 0)  # fill alpha
  expect_gt(grepRaw("/CA 0.8", raw, fixed = TRUE), 0)  # stroke alpha
})

test_that("grid.glyph renders glyphs from an explicit font file", {
  skip_if(getRversion() < "4.3.0")
  skip_if_not(exists("glyphInfo", envir = asNamespace("grid")))
  font <- grDevices::glyphFont(systemfonts::match_fonts("sans")$path,
                               index = 0L, family = "sans", weight = "normal",
                               style = "normal")
  fonts <- grDevices::glyphFontList(sans = font)
  gid <- systemfonts::glyph_info("A", family = "sans")$index
  file <- render_grid(
    grid::grid.glyph(grDevices::glyphInfo(as.integer(gid), x = 0.5, y = 0.5,
                                     font = 1L, size = 40, fontList = fonts,
                                     width = 0.1, height = 0.1))
  )
  content <- paste(pdf_streams(file), collapse = "\n")
  expect_match(content, " Tj\n", fixed = TRUE)
  expect_gt(grepRaw("/FontFile2", raw_pdf(file), fixed = TRUE), 0)
})

test_that("closing a device with no pages writes a valid one-page PDF", {
  file <- tempfile(fileext = ".pdf")
  portable_pdf(file)
  dev.off()
  raw <- raw_pdf(file)
  expect_equal(rawToChar(raw[1:5]), "%PDF-")
  expect_gt(grepRaw("/Type /Pages /Count 1 ", raw, fixed = TRUE), 0)
})

test_that("viewport clipping works with deviceClip enabled", {
  skip_if(getRversion() < "4.2.0")
  # Rotated viewport clips are a grid-level limitation on every device
  expect_warning(
    render_grid({
      grid::pushViewport(grid::viewport(angle = 30, clip = TRUE))
      grid::grid.rect(gp = grid::gpar(fill = "grey90"))
      grid::popViewport()
    }),
    "cannot clip to rotated viewport"
  )
  # Ordinary rectangular viewport clips go straight to the device
  file <- render_grid({
    grid::pushViewport(grid::viewport(x = 0.25, width = 0.5, clip = TRUE))
    grid::grid.rect(gp = grid::gpar(fill = "grey90"))
    grid::popViewport()
  })
  content <- paste(pdf_streams(file), collapse = "\n")
  expect_match(content, " re W n", fixed = TRUE)
})

test_that("dash patterns are adjusted for non-butt line caps", {
  # lwd = 1 -> 0.75 pt; lty = "44" (dashed)
  file <- render_pdf({
    plot.new()
    plot.window(c(0, 1), c(0, 1))
    segments(0.1, 0.5, 0.9, 0.5, lty = "44", lend = "round")
  })
  content <- paste(pdf_streams(file), collapse = "\n")
  # on segment shortened by 1 lwd, off lengthened by 1: [3 5] * 0.75
  expect_match(content, "[2.25 3.75 ] 0 d", fixed = TRUE)

  # Butt caps leave the pattern unadjusted
  file <- render_pdf({
    plot.new()
    plot.window(c(0, 1), c(0, 1))
    segments(0.1, 0.5, 0.9, 0.5, lty = "44", lend = "butt")
  })
  content <- paste(pdf_streams(file), collapse = "\n")
  expect_match(content, "[3 3 ] 0 d", fixed = TRUE)
})

test_that("dotted lines with round caps become solid zero-length dashes", {
  file <- render_pdf({
    plot.new()
    plot.window(c(0, 1), c(0, 1))
    segments(0.1, 0.5, 0.9, 0.5, lty = "11", lend = "round")
  })
  content <- paste(pdf_streams(file), collapse = "\n")
  # on segment clamped to zero length; round caps render it as dots
  expect_match(content, "[0 1.5 ] 0 d", fixed = TRUE)
  expect_match(content, "1 J", fixed = TRUE)
})

test_that("semi-transparent page backgrounds get an alpha gstate", {
  file <- render_pdf(plot.new(), bg = "#FF000080")
  content <- paste(pdf_streams(file), collapse = "\n")
  expect_match(content, "/GS1 gs\n0 0 ", fixed = TRUE)
  raw <- raw_pdf(file)
  expect_gt(grepRaw("/ca 0.502", raw, fixed = TRUE), 0)
})

test_that("rotated rasters pivot about the anchor corner", {
  file <- tempfile(fileext = ".pdf")
  portable_pdf(file)
  grid::grid.newpage()
  grid::pushViewport(grid::viewport(angle = 45))
  grid::grid.raster(matrix(c("#FF0000", "#0000FF"), nrow = 1),
                    x = grid::unit(0.2, "npc"), y = grid::unit(0.2, "npc"),
                    width = grid::unit(0.4, "npc"),
                    height = grid::unit(0.4, "npc"),
                    just = c("left", "bottom"))
  dev.off()
  content <- paste(pdf_streams(file), collapse = "\n")
  # the image is scaled from the origin of the rotated frame (i.e. from the
  # anchor corner), not offset by half the image size
  expect_match(content, "201.6 0 0 201.6 0 0 cm\n", fixed = TRUE)
})

test_that("patterns map device units to page space when scaling != 1", {
  skip_if(getRversion() < "4.2.0")
  file <- render_grid(
    grid::grid.rect(gp = grid::gpar(
      fill = grid::linearGradient(c("red", "blue")))),
    scaling = 2
  )
  raw <- raw_pdf(file)
  expect_gt(grepRaw("/Matrix [0.5 0 0 0.5 0 0]", raw, fixed = TRUE), 0)
})

test_that("alpha-gradient fills do not mask the stroke", {
  skip_if(getRversion() < "4.2.0")
  file <- render_grid(
    grid::grid.rect(gp = grid::gpar(
      fill = grid::linearGradient(c("#FF000080", "#0000FF80")),
      col = "#00FF00FF"))
  )
  content <- paste(pdf_streams(file), collapse = "\n")
  # fill and stroke are painted in separate passes; the soft-mask gstate is
  # only applied to the fill pass
  expect_match(content, "re\nf\nQ", fixed = TRUE)
  expect_match(content, "re\nS\nQ", fixed = TRUE)
  expect_no_match(content, "re\nB", fixed = TRUE)
})

test_that("alpha gradients combine with an active user mask", {
  skip_if(getRversion() < "4.2.0")
  file <- tempfile(fileext = ".pdf")
  portable_pdf(file)
  grid::grid.newpage()
  grid::pushViewport(grid::viewport(
    mask = grid::circleGrob(r = 0.4, gp = grid::gpar(fill = "black"))))
  grid::grid.rect(gp = grid::gpar(
    fill = grid::linearGradient(c("#FF000080", "#0000FF80"))))
  dev.off()
  streams <- pdf_streams(file)
  # a soft-mask form paints the alpha shading through the user mask's gstate
  combined <- Filter(function(s) {
    grepl(" gs\n/", s, fixed = TRUE) && grepl(" sh", s, fixed = TRUE)
  }, streams)
  expect_length(combined, 1)
})

test_that("grid.newpage releases all patterns, masks, clip paths, and groups", {
  skip_if(getRversion() < "4.2.0")
  file <- tempfile(fileext = ".pdf")
  portable_pdf(file)
  grid::grid.newpage()
  grid::pushViewport(grid::viewport(
    mask = grid::circleGrob(r = 0.4, gp = grid::gpar(fill = "black"))))
  grid::grid.rect(gp = grid::gpar(
    fill = grid::linearGradient(c("red", "blue"))))
  grid::grid.newpage()
  expect_no_error(grid::grid.rect(gp = grid::gpar(fill = "orange")))
  grid::grid.newpage()
  expect_no_error(grid::grid.rect(gp = grid::gpar(
    fill = grid::linearGradient(c("red", "blue")))))
  dev.off()
  raw <- raw_pdf(file)
  expect_gt(grepRaw("/Count 3", raw, fixed = TRUE), 0)
})

test_that("ligature glyphs map to their source characters in ToUnicode", {
  file <- render_pdf({
    plot.new()
    text(0.5, 0.5, "fi", cex = 3)
  })
  # the ToUnicode CMap stream is written uncompressed
  raw <- raw_pdf(file)
  expect_gt(grepRaw("beginbfchar", raw, fixed = TRUE), 0)
  # multi-codepoint value: the ligature copies as "fi"
  expect_gt(grepRaw("<00660069>", raw, fixed = TRUE), 0)
})

test_that("contextual forms (Arabic) get per-character ToUnicode entries", {
  file <- render_pdf({
    plot.new()
    text(0.5, 0.5, "\u0645\u0631\u062D\u0628\u0627", cex = 3)  # "مرحبا"
  })
  raw <- raw_pdf(file)
  # every source character is recoverable; no empty mappings
  for (cp in c("0627", "0628", "062D", "0631", "0645")) {
    expect_gt(grepRaw(paste0("<", cp, ">"), raw, fixed = TRUE), 0)
  }
  expect_length(grepRaw("> <>", raw, fixed = TRUE), 0)
})

test_that("text is tagged with an /ActualText span holding the source string", {
  file <- render_pdf({
    plot.new()
    text(0.5, 0.5, "fi", cex = 3)
    text(0.5, 0.2, "x\U0001F600y", cex = 3)
  })
  content <- paste(pdf_streams(file), collapse = "\n")
  # exact source text per span, including what ToUnicode cannot express
  # (emoji images, reused glyphs)
  expect_match(content, "/Span <</ActualText <00660069>>> BDC", fixed = TRUE)
  expect_match(content, "/Span <</ActualText <0078D83DDE000079>>> BDC",
               fixed = TRUE)
  expect_match(content, "ET\nEMC\nQ", fixed = TRUE)
})
