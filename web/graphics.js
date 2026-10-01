// The runtime copies raw CGA indices and four RGB triples out of the wasm
// heap. The canvas keeps the original 320/640x200 pixels; CSS gives it the
// same 640x400 natural display area as the 80x25, 8x16 text screen.
export function createGraphics(canvas, container) {
  const context = canvas.getContext("2d", { alpha: false });
  if (!context) throw new Error("CGA graphics need a 2D canvas");
  let image;

  return {
    draw(frame) {
      if (!frame) {
        canvas.hidden = true;
        container.classList.remove("graphics-mode");
        container.setAttribute("aria-label", "Volkov Commander, 80 columns by 25 rows");
        return;
      }
      const { width, height, pixels, palette } = frame;
      if ((width !== 320 && width !== 640) || height !== 200
          || pixels.length !== width * height || palette.length !== 12) {
        throw new RangeError("Invalid CGA frame");
      }
      if (!image || image.width !== width || image.height !== height) {
        canvas.width = width;
        canvas.height = height;
        image = context.createImageData(width, height);
        context.imageSmoothingEnabled = false;
      }
      for (let pixel = 0; pixel < pixels.length; pixel++) {
        const colour = pixels[pixel] * 3;
        const rgba = pixel * 4;
        image.data[rgba] = palette[colour];
        image.data[rgba + 1] = palette[colour + 1];
        image.data[rgba + 2] = palette[colour + 2];
        image.data[rgba + 3] = 255;
      }
      context.putImageData(image, 0, 0);
      container.classList.add("graphics-mode");
      container.setAttribute("aria-label", `CGA graphics, ${width} by ${height} pixels`);
      canvas.hidden = false;
    },
  };
}
