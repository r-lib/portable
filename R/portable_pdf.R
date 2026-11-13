#' Create a PDF device with embedded fonts
#'
#' `portable_pdf()` opens a PDF graphics device that uses systemfonts and
#' textshaping for font lookup and text layout (the same font stack as the
#' ragg devices) and embeds all used fonts so the document renders the same on
#' any machine.
#'
#' @param file Path of the file to write to. As with the ragg and svglite
#'   devices, the path may contain a `sprintf()`-style integer format such as
#'   `"%03d"`: when it does, each page is written to its own file with the
#'   pattern expanded to the page number (starting at 1). Without a format,
#'   all pages are written to the single named file. Use `"%%"` for a literal
#'   `%` when using a format.
#' @param width,height Size of the device in inches.
#' @param scaling A scaling factor applied to the device resolution, in the
#'   same way as the `scaling` argument in ragg and the `scale` argument in
#'   svglite. Values above 1 increase the size of text and other elements
#'   relative to the physical page size.
#' @param pointsize Default point size of text.
#' @param family Default font family to use for text.
#' @param bg Background colour of the device. Use `"transparent"` for no fill.
#' @param metadata Document metadata, created with [pdf_metadata()].
#' @param colormodel Colour model for output: `"rgb"` (the default), `"gray"`,
#'   or `"cmyk"`. Applies to vector graphics and embedded raster images alike.
#' @param compress Whether to compress the PDF content streams. Set to `FALSE`
#'   to get human-readable content streams, e.g. for debugging the output.
#'   Embedded font and image data is always compressed.
#'
#' @return Invisibly, the path to the created file.
#'
#' @examples
#' portable_pdf(tempfile(fileext = ".pdf"))
#' plot(sin, -pi, 2 * pi)
#' dev.off()
#'
#' @export
portable_pdf <- function(file = "portable.pdf", width = 7, height = 7,
                         scaling = 1, pointsize = 12, family = "sans",
                         bg = "white", metadata = pdf_metadata(),
                         colormodel = c("rgb", "gray", "cmyk"),
                         compress = TRUE) {
  colormodel <- arg_match(colormodel)
  if (!isTRUE(compress) && !identical(compress, FALSE)) {
    cli_abort("{.arg compress} must be `TRUE` or `FALSE`.")
  }
  check_string(file)
  check_number_decimal(width, min = 0, allow_infinite = FALSE)
  check_number_decimal(height, min = 0, allow_infinite = FALSE)
  check_number_decimal(scaling, min = 0, allow_infinite = FALSE)
  check_number_decimal(pointsize, min = 0, allow_infinite = FALSE)
  check_string(family)
  if (!inherits(metadata, "portable_pdf_metadata")) {
    cli_abort(
      "{.arg metadata} must be created with {.fn pdf_metadata}"
    )
  }
  file <- path.expand(file)
  # A sprintf-style integer format (e.g. "%03d") switches to one file per page
  one_file_per_page <- grepl("%[-+ #0]*[0-9]*[di]", gsub("%%", "", file))
  bg_col <- grDevices::col2rgb(bg, alpha = TRUE)
  bg_rgba <- as.integer(bg_col[, 1])
  invisible(.Call("C_portable_pdf", file, as.double(width),
                  as.double(height), as.double(scaling), as.double(pointsize),
                  family, bg_rgba, metadata$title, metadata$author,
                  metadata$creator, metadata$subject, metadata$keywords,
                  metadata$creation_date, colormodel, one_file_per_page,
                  compress, PACKAGE = "portable"))
}
