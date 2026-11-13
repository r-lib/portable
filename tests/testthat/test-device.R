test_that("the device produces a valid PDF file", {
  file <- render_pdf(plot(1:10))
  raw <- readBin(file, "raw", file.size(file))
  expect_equal(rawToChar(raw[1:5]), "%PDF-")
  expect_equal(rawToChar(tail(raw, 6)), "%%EOF\n")
})

test_that("fonts are embedded and subset", {
  file <- render_pdf(plot(1:10, main = "title"))
  raw <- readBin(file, "raw", file.size(file))
  expect_gt(grepRaw("/FontFile2", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("/Identity-H", raw, fixed = TRUE), 0)
  expect_gt(grepRaw("beginbfchar", raw, fixed = TRUE), 0)  # ToUnicode CMap
  # Embedded font carries a 6-letter subset tag before its (platform-
  # dependent) PostScript name
  expect_gt(grepRaw("[A-Z]{6}\\+", raw), 0)
  # Subsetting keeps a one-font document small
  expect_lt(file.size(file), 100 * 1024)
})

test_that("multiple pages are written", {
  file <- render_pdf({
    plot(1:10)
    plot(10:1)
  })
  raw <- readBin(file, "raw", file.size(file))
  expect_gt(grepRaw("/Type /Pages /Count 2 ", raw, fixed = TRUE), 0)
})

test_that("rasters are embedded as images", {
  file <- render_pdf({
    plot.new()
    plot.window(c(0, 6), c(0, 6))
    rasterImage(as.raster(matrix(hcl.colors(16), 4)), 1, 1, 5, 5)
  })
  raw <- readBin(file, "raw", file.size(file))
  expect_gt(grepRaw("/Subtype /Image", raw, fixed = TRUE), 0)
})

test_that("scaling changes device units but not the physical page size", {
  f1 <- render_pdf(plot(1:10), scaling = 1)
  f2 <- render_pdf(plot(1:10), scaling = 2)
  for (f in c(f1, f2)) {
    raw <- readBin(f, "raw", file.size(f))
    # 7in x 7in at 72 pt/in regardless of scaling
    expect_gt(grepRaw("/MediaBox [0 0 504 504]", raw, fixed = TRUE), 0)
  }
  streams1 <- pdf_streams(f1)
  streams2 <- pdf_streams(f2)
  # The base CTM maps device units to points
  expect_true(any(grepl("^1 0 0 1 0 0 cm", streams1)))
  expect_true(any(grepl("^0.5 0 0 0.5 0 0 cm", streams2)))
})

test_that("text is drawn as positioned glyphs from the embedded font", {
  file <- render_pdf(plot(1:10, main = "abc"))
  streams <- pdf_streams(file)
  content <- paste(streams, collapse = "\n")
  expect_match(content, "BT", fixed = TRUE)
  expect_match(content, " Tf", fixed = TRUE)
  expect_match(content, " Tj", fixed = TRUE)
})

test_that("the background colour fills the page", {
  file <- render_pdf(plot(1:10), bg = "lightyellow")
  streams <- pdf_streams(file)
  # lightyellow = rgb(255, 255, 224); numbers are emitted trimmed
  expect_true(any(grepl("1 1 0.8784 rg", streams, fixed = TRUE)))
})

test_that("compress = FALSE leaves content streams human-readable", {
  file <- render_pdf(plot(1:10), compress = FALSE)
  raw <- readBin(file, "raw", file.size(file))
  # Only the embedded font stream is still compressed
  expect_length(grepRaw("/FlateDecode", raw, all = TRUE, fixed = TRUE), 1)
  # The page content stream is plain text
  expect_gt(grepRaw("0 0 504 504 re f", raw, fixed = TRUE), 0)
})

test_that("invalid dimensions are rejected", {
  expect_error(portable_pdf(tempfile(fileext = ".pdf"), width = 0))
  expect_error(portable_pdf(tempfile(fileext = ".pdf"), scaling = -1))
  expect_error(portable_pdf(tempfile(fileext = ".pdf"), width = NA))
  expect_error(portable_pdf(tempfile(fileext = ".pdf"), height = NaN))
  expect_error(portable_pdf(tempfile(fileext = ".pdf"), width = 1:2))
  expect_error(portable_pdf(tempfile(fileext = ".pdf"), family = c("a", "b")))
})
