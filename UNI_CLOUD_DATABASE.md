# Uni-Cloud Database Notes

## Collection: `app_playback_instances`

This collection stores generated playback alarm instances for device pull, local ringing, and playback state reporting.

### Key fields

- `clientId`: client identifier used by the device request payload.
- `instanceId`: unique playback instance identifier.
- `alarmId`: logical alarm identifier.
- `deviceId`: target device identifier.
- `ringAt`: ISO datetime used by the device for due-time parsing.
- `ringDateKey`: date key used for cloud-side grouping or filtering.
- `time`: display time text from the cloud record.
- `period`: time period text such as `上午` or `下午`.
- `title`: alarm title shown to the user.
- `voice`: selected voice profile.
- `nickname`: target nickname used in generated script.
- `repeat`: repeat rule text.
- `status`: playback task status, such as `pending`.
- `scriptStatus`: script generation status.
- `audioStatus`: audio generation status.
- `audioDownloadUrl`: signed `downloadPlaybackAudio` URL for ESP32 device-side validation, redirect, download, and playback.
- `fallbackMode`: fallback behavior when remote audio is unavailable.
- `createdAt`: creation timestamp.
- `updatedAt`: update timestamp.
- `generationError`: generation failure message, if any.
- `scriptLibraryId`: script template library identifier.
- `scriptProvider`: script generation provider.
- `scriptText`: generated wake-up script content.
- `audioFileId`: generated audio file reference.
- `audioLibraryId`: audio library identifier.
- `audioProvider`: TTS/audio generation provider.

## Sample document

```json
{
  "clientId": "client_1776770443964_jmxlxc",
  "instanceId": "instance_alarm_1776770757934_20260421233000000",
  "alarmId": "alarm_1776770757934",
  "deviceId": "dev_demo_001",
  "ringAt": "2026-04-24T21:14:00.000Z",
  "ringDateKey": "2026-04-24",
  "time": "9:11",
  "period": "下午",
  "title": "测试",
  "voice": "温柔派",
  "nickname": "人哥哥",
  "repeat": "每天",
  "status": "pending",
  "scriptStatus": "generated",
  "audioStatus": "generated",
  "audioDownloadUrl": "https://alarm.yoyospace.cc/device-service/downloadPlaybackAudio?clientId=client_1776770443964_jmxlxc&deviceId=dev_demo_001&instanceId=instance_alarm_1776770757934_20260421233000000&exp=1776771000&sig=...",
  "fallbackMode": "device_local_realtime_mix",
  "createdAt": 1776770758119,
  "updatedAt": 1776770819259,
  "generationError": "",
  "scriptLibraryId": "69e75eea189f86efafe680cf",
  "scriptProvider": "ark",
  "scriptText": "人哥哥，早上好呀。新的一天正轻轻敲着你的窗，等着你给它一个温暖的拥抱呢。先别急着动，让意识像小蝴蝶一样慢慢飞回来——对啦，轻轻动一动手指，再慢慢睁开眼，让窗帘缝里溜进来的阳光，一点一点把你叫醒吧。 今天真是个被祝福过的日子呢。我看了看窗外，天空蓝得像是水洗过一样，干干净净的，有几朵胖乎乎的云在慢悠悠地散步。气温也特别贴心，不冷不热的，你出门的话，记得在衬衫外面加一件薄薄的针织外套就刚刚好啦。要是待会儿要出门，听说早高峰的车流还有点调皮，咱们不跟它赛跑，安全第一，慢慢来就好。 我会一直在这儿，像窗台上那盆绿植一样安静地陪着你。准备好了吗？让我们一起，打开这扇门，走进这温暖又明亮的一天吧。快醒醒啦，我亲爱的人哥哥。",
  "audioFileId": "https://mp-569ac274-a245-482f-994d-e065e5e73b0b.cdn.bspapp.com/cloudstorage/9782c899-9824-4fdf-97a5-7bbbf12d537d.mp3",
  "audioLibraryId": "69e75f03816a3fbd38c88345",
  "audioProvider": "doubao_tts_v3"
}
```
```json
{
    "clientId": "client_1776770443964_jmxlxc",
    "instanceId": "instance_alarm_1776770757934_20260421233000000",
    "alarmId": "alarm_1776845645148",
    "deviceId": "dev_demo_001",
    "ringAt": "2026-04-22T23:30:00.000Z",
    "ringDateKey": "2026-04-23",
    "time": "07:30",
    "period": "上午",
    "title": "起床",
    "voice": "温柔派",
    "nickname": "航哥哥",
    "repeat": "每天",
    "status": "pending",
    "scriptStatus": "generated",
    "audioStatus": "generated",
    "audioUrl": "https://mp-569ac274-a245-482f-994d-e065e5e73b0b.cdn.bspapp.com/cloudstorage/ea5ad816-6f7e-4464-841f-ab0d45123d57.mp3",
    "fallbackMode": "device_local_realtime_mix",
    "createdAt": 1776845645415,
    "updatedAt": 1776845697373,
    "generationError": "",
    "scriptLibraryId": "69e8836f7ae70807806e4953",
    "scriptProvider": "ark",
    "scriptText": "航哥哥，早上好呀。新的一天正温柔地等着你呢，该起床啦。昨晚睡得香不香呀？先别急着起来，轻轻伸个懒腰，活动一下手脚，再慢慢坐起身，让身体一点点苏醒过来。 今天外面天气可好啦，阳光暖暖的，像在轻轻拥抱你。不过早上出门的时候，记得在包里放件薄外套哦，早晚的凉意还是会偷偷溜过来的。要是你待会儿要去远些的地方，出门前记得抬头看看云朵多不多，再决定要不要带把伞，这样更安心。 好啦，慢慢来，先喝杯温水润润喉咙吧。早餐已经准备好啦，是你喜欢的味道。希望你今天每一步都走得从容，每一刻都过得开心。航哥哥，崭新的一天，要好好开始哦。",
    "audioFileId": "https://mp-569ac274-a245-482f-994d-e065e5e73b0b.cdn.bspapp.com/cloudstorage/ea5ad816-6f7e-4464-841f-ab0d45123d57.mp3",
    "audioLibraryId": "69e88381bd022074bc4e6ef4",
    "audioProvider": "doubao_tts_v3"
}
```