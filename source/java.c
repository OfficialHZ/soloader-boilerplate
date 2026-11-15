/*
 * Minecraft: Story Mode - PS Vita loader
 *
 * java.c - Fake answers for the Java methods the engine calls through JNI.
 *
 * On Android the engine asks its Java side (TelltaleActivity / SDLActivity)
 * for things like the device model, storage folders and the OBB file name.
 * FalsoJNI pretends to be Java; this file says what each method returns.
 *
 * Every call is written to log.txt so we can see what the engine asks for.
 */

#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include <psp2/kernel/clib.h>
#include <stdarg.h>

extern void log_raw(const char *text);

static void jlog(const char *fmt, ...) {
    char line[384];
    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(line, sizeof(line), fmt, list);
    va_end(list);
    log_raw(line);
}

// Reads a Java string argument into a C buffer (safe for NULL).
static void jstring_to_c(jstring s, char *out, size_t size) {
    out[0] = '\0';
    if (!s) return;
    const char *c = (*(&jni))->GetStringUTFChars(&jni, s, NULL);
    if (c) {
        sceClibStrncpy(out, c, size - 1);
        out[size - 1] = '\0';
        (*(&jni))->ReleaseStringUTFChars(&jni, s, c);
    }
}

/* ---------------------------------------------------------------------- */
/* Method implementations                                                 */
/* ---------------------------------------------------------------------- */

// "getHardwareModel", "()Ljava/lang/String;"
static jobject getHardwareModel(jmethodID id, va_list args) {
    jlog("[java] getHardwareModel() -> \"PS Vita\"\n");
    return (*(&jni))->NewStringUTF(&jni, "PS Vita");
}

// "hasFeature", "(Ljava/lang/String;)Z"
static jboolean hasFeature(jmethodID id, va_list args) {
    char feature[128];
    jstring_to_c(va_arg(args, jstring), feature, sizeof(feature));
    jlog("[java] hasFeature(\"%s\") -> false\n", feature);
    return JNI_FALSE;
}

// "setFramebufferSize", "(II)V"
static void setFramebufferSize(jmethodID id, va_list args) {
    int w = va_arg(args, int);
    int h = va_arg(args, int);
    jlog("[java] setFramebufferSize(%d, %d)\n", w, h);
}

// "getExternalStorageDirs", "()[Ljava/lang/String;"
static jobject getExternalStorageDirs(jmethodID id, va_list args) {
    jlog("[java] getExternalStorageDirs() -> [\"ux0:data/mcsm\"]\n");
    jobjectArray arr = (*(&jni))->NewObjectArray(&jni, 1, NULL, NULL);
    (*(&jni))->SetObjectArrayElement(&jni, arr, 0, (*(&jni))->NewStringUTF(&jni, "ux0:data/mcsm"));
    return arr;
}

// "getObbFileName", "(Z)Ljava/lang/String;"  (true = main OBB, false = patch OBB)
static jobject getObbFileName(jmethodID id, va_list args) {
    int main_obb = va_arg(args, int);
    const char *name = main_obb ? "ux0:data/mcsm/main.obb" : "ux0:data/mcsm/patch.obb";
    jlog("[java] getObbFileName(%s) -> \"%s\"\n", main_obb ? "main" : "patch", name);
    return (*(&jni))->NewStringUTF(&jni, name);
}

// "requestPermission" - just log it, everything is allowed on the Vita
static void requestPermission(jmethodID id, va_list args) {
    jlog("[java] requestPermission()\n");
}

// "openURL", "(Ljava/lang/String;)V"
static void openURL(jmethodID id, va_list args) {
    char url[256];
    jstring_to_c(va_arg(args, jstring), url, sizeof(url));
    jlog("[java] openURL(\"%s\") - ignored\n", url);
}

// "openAuthDialog" - online accounts don't exist anymore
static void openAuthDialog(jmethodID id, va_list args) {
    jlog("[java] openAuthDialog() - ignored\n");
}

// "updatePurchases" - store is gone; ignore
static void updatePurchases(jmethodID id, va_list args) {
    jlog("[java] updatePurchases() - ignored\n");
}

// "getXDPI" / "getYDPI", "()F" - screen pixels per inch.
// The Vita's 5-inch 960x544 screen is about 220 ppi.
static jfloat getXDPI(jmethodID id, va_list args) {
    jlog("[java] getXDPI() -> 220\n");
    return 220.0f;
}

static jfloat getYDPI(jmethodID id, va_list args) {
    jlog("[java] getYDPI() -> 220\n");
    return 220.0f;
}

// "getSampleRate", "()I" - audio output rate
static jint getSampleRate(jmethodID id, va_list args) {
    jlog("[java] getSampleRate() -> 48000\n");
    return 48000;
}

// "getOutputFramesPerBuffer", "()I" - audio buffer size
static jint getOutputFramesPerBuffer(jmethodID id, va_list args) {
    jlog("[java] getOutputFramesPerBuffer() -> 256\n");
    return 256;
}

// "isUsingBluetooth", "()Z"
static jboolean isUsingBluetooth(jmethodID id, va_list args) {
    return JNI_FALSE;
}

// "getHardwareDisplay", "()Ljava/lang/String;"
static jobject getHardwareDisplay(jmethodID id, va_list args) {
    jlog("[java] getHardwareDisplay() -> \"PS Vita\"\n");
    return (*(&jni))->NewStringUTF(&jni, "PS Vita");
}

// "getHardwareManufacturer", "()Ljava/lang/String;"
static jobject getHardwareManufacturer(jmethodID id, va_list args) {
    jlog("[java] getHardwareManufacturer() -> \"Sony\"\n");
    return (*(&jni))->NewStringUTF(&jni, "Sony");
}

// "getHardwareOS", "()Ljava/lang/String;"
static jobject getHardwareOS(jmethodID id, va_list args) {
    jlog("[java] getHardwareOS() -> \"PS Vita\"\n");
    return (*(&jni))->NewStringUTF(&jni, "PS Vita");
}

// "getHardwareBoard", "()Ljava/lang/String;"
static jobject getHardwareBoard(jmethodID id, va_list args) {
    jlog("[java] getHardwareBoard() -> \"vita\"\n");
    return (*(&jni))->NewStringUTF(&jni, "vita");
}

// org.fmod.FMOD (libfmod.so's own Java helper class)
// "supportsLowLatency", "()Z"
static jboolean supportsLowLatency(jmethodID id, va_list args) {
    jlog("[java] FMOD supportsLowLatency() -> false\n");
    return JNI_FALSE;
}

// "checkInit", "()Z" - true once org.fmod.FMOD.init(context) ran in Java
static jboolean checkInit(jmethodID id, va_list args) {
    return JNI_TRUE;
}

// "getOutputSampleRate", "()I"
static jint getOutputSampleRate(jmethodID id, va_list args) {
    jlog("[java] FMOD getOutputSampleRate() -> 48000\n");
    return 48000;
}

// "getOutputBlockSize", "()I"
static jint getOutputBlockSize(jmethodID id, va_list args) {
    jlog("[java] FMOD getOutputBlockSize() -> 256\n");
    return 256;
}

/* ---------------------------------------------------------------------- */
/* Method tables                                                          */
/* ---------------------------------------------------------------------- */

NameToMethodID nameToMethodId[] = {
    { 100, "getHardwareModel",       METHOD_TYPE_OBJECT },
    { 101, "hasFeature",             METHOD_TYPE_BOOLEAN },
    { 102, "setFramebufferSize",     METHOD_TYPE_VOID },
    { 103, "getExternalStorageDirs", METHOD_TYPE_OBJECT },
    { 104, "getObbFileName",         METHOD_TYPE_OBJECT },
    { 105, "requestPermission",      METHOD_TYPE_VOID },
    { 106, "openURL",                METHOD_TYPE_VOID },
    { 107, "openAuthDialog",         METHOD_TYPE_VOID },
    { 108, "updatePurchases",        METHOD_TYPE_VOID },
    { 109, "getXDPI",                METHOD_TYPE_FLOAT },
    { 110, "getYDPI",                METHOD_TYPE_FLOAT },
    { 111, "getSampleRate",          METHOD_TYPE_INT },
    { 112, "getOutputFramesPerBuffer", METHOD_TYPE_INT },
    { 113, "isUsingBluetooth",       METHOD_TYPE_BOOLEAN },
    { 114, "getHardwareDisplay",     METHOD_TYPE_OBJECT },
    { 115, "getHardwareManufacturer", METHOD_TYPE_OBJECT },
    { 116, "supportsLowLatency",     METHOD_TYPE_BOOLEAN },
    { 117, "getOutputSampleRate",    METHOD_TYPE_INT },
    { 118, "getOutputBlockSize",     METHOD_TYPE_INT },
    { 119, "checkInit",              METHOD_TYPE_BOOLEAN },
    { 120, "getHardwareOS",          METHOD_TYPE_OBJECT },
    { 121, "getHardwareBoard",       METHOD_TYPE_OBJECT },
};

MethodsBoolean methodsBoolean[] = {
    { 101, hasFeature },
    { 113, isUsingBluetooth },
    { 116, supportsLowLatency },
    { 119, checkInit },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {
    { 109, getXDPI },
    { 110, getYDPI },
};
MethodsInt methodsInt[] = {
    { 111, getSampleRate },
    { 112, getOutputFramesPerBuffer },
    { 117, getOutputSampleRate },
    { 118, getOutputBlockSize },
};
MethodsLong methodsLong[] = {};
MethodsObject methodsObject[] = {
    { 100, getHardwareModel },
    { 103, getExternalStorageDirs },
    { 104, getObbFileName },
    { 114, getHardwareDisplay },
    { 115, getHardwareManufacturer },
    { 120, getHardwareOS },
    { 121, getHardwareBoard },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    { 102, setFramebufferSize },
    { 105, requestPermission },
    { 106, openURL },
    { 107, openAuthDialog },
    { 108, updatePurchases },
};

/* ---------------------------------------------------------------------- */
/* Fields                                                                 */
/* ---------------------------------------------------------------------- */

// System-wide constant that applications sometimes request
// https://developer.android.com/reference/android/content/Context.html#WINDOW_SERVICE
char WINDOW_SERVICE[] = "window";

// Android version the engine thinks it runs on
// https://developer.android.com/reference/android/os/Build.VERSION.html#SDK_INT
const int SDK_INT = 19; // Android 4.4 / KitKat

NameToFieldID nameToFieldId[] = {
		{ 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
		{ 1, "SDK_INT", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
		{ 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
		{ 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
