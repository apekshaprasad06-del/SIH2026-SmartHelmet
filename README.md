# Smart Helmet / RideGuard

Android Studio Kotlin project for ESP32 Bluetooth smart-helmet monitoring.

## Current behavior
- Crash Detected counter with confirmation popup before reset.
- False Alarm counter with confirmation popup before reset.
- Counters reset automatically when the Indian calendar day changes (Asia/Kolkata).
- No acceleration UI or acceleration data parsing.
- Gyroscope fields remain backend-only.
- Alcohol is displayed as Yes/No.
- Alcohol state changes are recorded in the notification panel with Indian date/time.
- A new alcohol detection (`ALCOHOL=Yes`, including the first Yes packet after connection) immediately sends an SMS to all saved emergency contacts.
- A new crash event that increments the crash counter immediately sends an SMS to all saved emergency contacts.
- A new false-alarm event that increments the false-alarm counter immediately sends an SMS to all saved emergency contacts.
- SMS sending is automatic; there is no test-SMS button and no confirmation dialog.
- Repeated packets for the same active alarm/alcohol state do not send repeated SMS messages. A new event/state transition is required, preventing SMS spam.

## ESP32 packet examples
Normal:
`ALCOHOL=No,LAT=20.2961,LON=85.8245,SPEED=42,CRASH=0,FAKE_ALARM=0,FORCE=0.0 g`

Crash:
`ALCOHOL=No,LAT=20.2961,LON=85.8245,SPEED=42,CRASH=1,FAKE_ALARM=0,FORCE=5.8 g`

False alarm:
`ALCOHOL=No,LAT=20.2961,LON=85.8245,SPEED=30,CRASH=0,FAKE_ALARM=1,FORCE=0.0 g`

Alcohol:
`ALCOHOL=Yes,LAT=20.2961,LON=85.8245,SPEED=30,CRASH=0,FAKE_ALARM=0,FORCE=0.0 g`

Terminate each packet with `\\n` when using `SerialBT.println(...)`.

## SMS permission
Android must grant SEND_SMS permission. The app requests the permission at startup. If permission is denied, automatic SMS cannot be sent and the notification panel records the failure.

## Automatic emergency SMS troubleshooting

The app sends SMS directly with Android's `SEND_SMS` permission. It does not open the Messages app and it does not ask the user to confirm each alert.

Before testing:
1. Put a SIM with SMS service in the phone.
2. Grant **SMS / Send SMS** permission to Smart Helmet when Android asks.
3. Add at least one emergency contact in the app.
4. Make sure the phone can send a normal SMS from its default SMS subscription.
5. If SMS permission was previously denied with **Don't allow / Don't ask again**, open Android Settings → Apps → Smart Helmet → Permissions → SMS and enable it manually.
6. On dual-SIM phones, set a default SIM for SMS in Android system settings.

When an ESP32 alert arrives while SMS permission is missing, the app keeps the alert pending and automatically retries it after permission is granted.

SMS is triggered on a new event/transition to avoid sending hundreds of duplicate messages while the ESP32 repeatedly transmits the same state:
- Crash: `0 -> 1`
- False alarm: `0 -> 1`
- Alcohol: `No -> Yes`
