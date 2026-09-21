# Native code reaches all of this by name - FindClass, GetFieldID, GetMethodID,
# ThrowNew - so R8 sees no reference and would rename or strip it. The class
# names matter as much as the members: FindClass matches the literal string.

-keepclasseswithmembernames,includedescriptorclasses class ca.mpreg.imagedecoder.** {
    native <methods>;
}

# ptr, the private constructors and the exceptions' (String) constructor all
# have no Java-side caller.
-keep class ca.mpreg.imagedecoder.ImageDecoder { *; }
-keep class ca.mpreg.imagedecoder.ImageDecoder$Frame { *; }
-keep class ca.mpreg.imagedecoder.ImageDecoder$Gainmap { *; }
-keep class ca.mpreg.imagedecoder.ImageDecoder$DecodeException { *; }
-keep class ca.mpreg.imagedecoder.ImageDecoder$UnknownFormatException { *; }
-keep class ca.mpreg.imagedecoder.ImageDecoder$OutOfMemoryException { *; }
-keep class ca.mpreg.imagedecoder.ImageDecoder$NeedMoreDataException { *; }

# Both enums are wire values, not just types: fromNative matches native's
# get_name() against every constant. R8 otherwise drops the constants an app
# never mentions by name, and those formats silently decode as UNKNOWN.
-keep class ca.mpreg.imagedecoder.ImageDecoder$Format { *; }
-keep class ca.mpreg.imagedecoder.ImageDecoder$HdrKind { *; }
-keepclassmembers enum ca.mpreg.imagedecoder.** {
    public static **[] values();
    public static ** valueOf(java.lang.String);
}
