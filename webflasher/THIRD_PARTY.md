# Third-party code served with the installer

- `esptool-js-0.4.5.bundle.js` — [esptool-js](https://github.com/espressif/esptool-js) 0.4.5
  (Apache-2.0, Espressif Systems), the exact `bundle.js` npm published for that version
  (sha256 `ac4fab0d5613f4ee784f62b666c03d3d94572d5aa9f8c52ac6fb704208a2c911`). It bundles
  [pako](https://github.com/nodeca/pako) 2.1.0 (MIT AND Zlib). Served from this site rather
  than a CDN so the installer keeps working when the CDN does not, and so
  `tests/check_webflasher_split.py` can prove every method the page calls exists in the bytes
  actually served — the 0.9.79-NH page called `loader.after()`, which this version does not
  have, and reported a successful install as "Failed" while leaving the phone in the bootloader.
