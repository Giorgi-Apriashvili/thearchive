// Regenerates the site icons in web/public: favicon.svg, favicon.ico (16/32/48) and
// apple-touch-icon.png (180). Run `npm install && npm run build` here; needs rsvg-convert.
//
// The icon is the wordmark's accent in miniature: an italic Instrument Serif "A" — as in
// "Archive" — in the site's amber on its darkest ink. The letter is converted to outlines
// rather than set as SVG text, so it renders the same without the font installed. The
// typeface's hairlines vanish at 16 pixels, so the glyph is thickened with a stroke of its
// own colour; at that weight the counter of the A still stays open in a browser tab.

const opentype = require('opentype.js')
const { execFileSync } = require('child_process')
const fs = require('fs')
const path = require('path')

const FONT = path.join(
  __dirname,
  '../../web/node_modules/@fontsource/instrument-serif/files/instrument-serif-latin-400-italic.woff',
)
const OUT = path.join(__dirname, '../../web/public')
const INK = '#121519' // --color-ink-900
const AMBER = '#e0a458' // --color-accent

function svg({ rounded }) {
  const glyph = opentype.loadSync(FONT).getPath('A', 0, 0, 1000)
  const box = glyph.getBoundingBox()
  const height = 46
  const scale = height / (box.y2 - box.y1)
  const tx = (64 - (box.x2 - box.x1) * scale) / 2 - box.x1 * scale
  const ty = (64 - height) / 2 - box.y1 * scale
  return (
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">` +
    `<rect width="64" height="64"${rounded ? ' rx="14"' : ''} fill="${INK}"/>` +
    `<path transform="translate(${tx.toFixed(2)} ${ty.toFixed(2)}) scale(${scale.toFixed(5)})" ` +
    `fill="${AMBER}" stroke="${AMBER}" stroke-width="50" stroke-linejoin="round" ` +
    `d="${glyph.toPathData(1)}"/></svg>\n`
  )
}

function png(svgText, size) {
  return execFileSync('rsvg-convert', ['-w', String(size), '-h', String(size)], { input: svgText })
}

// An .ico holding PNGs, which every browser that still asks for /favicon.ico accepts.
function ico(images) {
  const header = Buffer.alloc(6 + 16 * images.length)
  header.writeUInt16LE(0, 0)
  header.writeUInt16LE(1, 2) // icon
  header.writeUInt16LE(images.length, 4)
  let offset = header.length
  images.forEach(({ size, data }, i) => {
    const e = 6 + 16 * i
    header.writeUInt8(size === 256 ? 0 : size, e)
    header.writeUInt8(size === 256 ? 0 : size, e + 1)
    header.writeUInt16LE(1, e + 4) // colour planes
    header.writeUInt16LE(32, e + 6) // bits per pixel
    header.writeUInt32LE(data.length, e + 8)
    header.writeUInt32LE(offset, e + 12)
    offset += data.length
  })
  return Buffer.concat([header, ...images.map((i) => i.data)])
}

const rounded = svg({ rounded: true })
fs.writeFileSync(path.join(OUT, 'favicon.svg'), rounded)
fs.writeFileSync(
  path.join(OUT, 'favicon.ico'),
  ico([16, 32, 48].map((size) => ({ size, data: png(rounded, size) }))),
)
// Square: iOS cuts its own rounded corners, and would leave ours as dark notches.
fs.writeFileSync(path.join(OUT, 'apple-touch-icon.png'), png(svg({ rounded: false }), 180))
console.log('wrote favicon.svg, favicon.ico, apple-touch-icon.png to web/public')
