# WebRTC
-keep class org.webrtc.** { *; }
-keep class org.webrtc.audio.** { *; }

# NanoHTTPD
-keep class fi.iki.elonen.** { *; }
-keep class org.nanohttpd.** { *; }

# Gson
-keep class com.google.gson.** { *; }
-keepattributes Signature
-keepattributes *Annotation*

# Native bridge
-keep class com.screencast.pro.NativeEncoder { *; }
-keep class com.screencast.pro.NativeEncoder$* { *; }

# Keep service
-keep class com.screencast.pro.RecordingService { *; }
-keep class com.screencast.pro.RecordingService$* { *; }
