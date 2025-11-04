function linearInterpolate(a, b, t) {
  return a + (b - a) * t;
}

function interpolateColor(rgb1, rgb2, t) {
  return `rgb(${Math.round(linearInterpolate(rgb1[0], rgb2[0], t))},` +
         `${Math.round(linearInterpolate(rgb1[1], rgb2[1], t))},` +
         `${Math.round(linearInterpolate(rgb1[2], rgb2[2], t))})`;
}

// Палитры из ColorBrewer, каждая точка - [R, G, B]
const RdYlBu = [
  [165, 0, 38],
  [215, 48, 39],
  [244, 109, 67],
  [253, 174, 97],
  [254, 224, 144],
  [224, 243, 248],
  [171, 217, 233],
  [116, 173, 209],
  [69, 117, 180],
  [49, 54, 149]
];

const RdYlGn = [
  [165, 0, 38],
  [215, 48, 39],
  [244, 109, 67],
  [253, 174, 97],
  [254, 224, 139],
  [217, 239, 139],
  [166, 217, 106],
  [102, 189, 99],
  [26, 152, 80],
  [0, 104, 55]
];

function makeInterpolator(palette) {
  return function(t) {
    const n = palette.length - 1;
    const scaledT = t * n;
    const i = Math.floor(scaledT);
    const localT = scaledT - i;

    if (i >= n) return `rgb(${palette[n][0]},${palette[n][1]},${palette[n][2]})`;
    return interpolateColor(palette[i], palette[i + 1], localT);
  };
}

// Создаем локальные функции, как в D3
const localInterpolateRdYlBu = makeInterpolator(RdYlBu);
const localInterpolateRdYlGn = makeInterpolator(RdYlGn);
