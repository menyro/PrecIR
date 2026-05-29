# Flipper Zero port

This port adds two pieces:

* `tools_python/precir2flipper.py`: generates Flipper-ready PrecIR job files.
* `flipperzero/precir`: a Flipper Zero external app that loads those job files and transmits them.

## What this port does

The Flipper app is optimized for the hardware it already has:

* image conversion, barcode handling, and frame building stay on the computer
* the Flipper only has to pick a prepared job file and transmit it

That keeps the device workflow simple and avoids entering long barcodes or image parameters on the Flipper itself.

## Files and folders

* App source: `/tmp/workspace/menyro/PrecIR/flipperzero/precir`
* Default generated jobs: `/tmp/workspace/menyro/PrecIR/flipperzero/jobs`
* Recommended SD card destination: `/ext/apps_data/precir/jobs`

## Step-by-step: build the Flipper app

1. Install `ufbt` on your computer:
   * `python -m pip install ufbt`
2. Change to the app directory:
   * `cd /tmp/workspace/menyro/PrecIR/flipperzero/precir`
3. Build the app:
   * `ufbt`
4. After a successful build, copy the generated `.fap` file to your Flipper using qFlipper or the SD card.

## Step-by-step: install the app on the Flipper

1. Connect the Flipper Zero to your computer.
2. Open qFlipper.
3. Copy the built `.fap` into the `Apps/Infrared` area on the Flipper, or drag the file into qFlipper and let it upload.
4. On the SD card, create this folder if it does not already exist:
   * `/ext/apps_data/precir/jobs`

## Step-by-step: generate a job file

### Image update

```bash
cd /tmp/workspace/menyro/PrecIR
python tools_python/precir2flipper.py image path/to/image.png 01234567890123456 --page 1 --color 0
```

### Segment display update

```bash
cd /tmp/workspace/menyro/PrecIR
python tools_python/precir2flipper.py segments 01234567890123456 00112233445566778899AABBCCDDEEFF00112233445566
```

### Raw frame

```bash
cd /tmp/workspace/menyro/PrecIR
python tools_python/precir2flipper.py raw 01234567890123456 DM 06C900000000 10
```

### Page change without a barcode

```bash
cd /tmp/workspace/menyro/PrecIR
python tools_python/precir2flipper.py page-dm 1 --duration 15
python tools_python/precir2flipper.py page-seg 1 --duration 15s
```

Generated jobs are written to `/tmp/workspace/menyro/PrecIR/flipperzero/jobs` by default unless `--output` is provided.

## Step-by-step: upload the job file

1. Copy the generated `.precir` file to:
   * `/ext/apps_data/precir/jobs`
2. Eject the Flipper cleanly.

## Step-by-step: use it on the Flipper

1. Open the **PrecIR** app on the Flipper.
2. Press **OK** or **RIGHT** to choose a `.precir` job file.
3. Use **UP/DOWN** to switch the transmit pin:
   * `Auto`: use external PA7 if detected, otherwise the internal IR LED
   * `Internal`: force the built-in IR LED
   * `GPIO PA7`: force the external IR output pin
4. Place the Flipper close to the ESL.
5. Press **OK** to transmit.
6. Wait for the status line to report completion.

## Notes

* `PP16` jobs are generated for image updates by default because they are much faster.
* `--pp4` can be used for image jobs if a specific ESL behaves better with the slower protocol.
* The Flipper app currently uses a 1 MHz carrier generated through the Flipper infrared HAL, which is the closest supported built-in setting to the original PrecIR transmitter design.
