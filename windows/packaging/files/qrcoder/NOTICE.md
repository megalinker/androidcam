# Vendored: QRCoder

Source: [codebude/QRCoder](https://github.com/codebude/QRCoder) tag **v1.4.3**, MIT
licensed (see `LICENSE.txt`). Vendored as source because the PhoneCam GUI is compiled
directly with `csc` (no NuGet restore) — see `../../assemble.ps1`.

Files are the upstream originals **except** `QRCodeGenerator.cs`, from which the four
`CreateQrCode(PayloadGenerator.Payload …)` / `GenerateQrCode(PayloadGenerator.Payload …)`
convenience overloads were removed. We only call `CreateQrCode(string, ECCLevel)`, and
those overloads dragged in `PayloadGenerator.cs` plus `System.Text.Encoding.CodePages`
and extra net40 shims we don't need. Nothing else was changed.

We use `PngByteQRCode` (pure managed, no System.Drawing dependency) to render the
pairing QR to PNG bytes, then load it into a `Bitmap` for WinForms.
