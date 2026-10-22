package drmprobe;

import android.content.pm.ApplicationInfo;
import android.media.MediaDrm;
import android.media.MediaDrmException;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.UUID;

/**
 * 查一次 Widevine 的 deviceUniqueId。
 *
 * app_process 里 ActivityThread.currentPackageName() 是 null，而 MediaDrm 只有
 * MediaDrm(UUID) 一个构造函数（内部会读 currentPackageName()），所以先伪造：
 *   ActivityThread.systemMain() -> mBoundApplication = new AppBindData()
 *   -> appInfo = new ApplicationInfo(); appInfo.packageName = "com.android.shell"
 * 之后 MediaDrm(WIDEVINE) 才能构造出来。
 *
 * 运行（root）：
 *   CLASSPATH=/data/local/tmp/drmprobe.jar app_process /system/bin drmprobe.DrmProbe
 */
public final class DrmProbe {
    private static final UUID WIDEVINE =
            new UUID(0xedef8ba979d64aceL, 0xa3c827dcd51d21edL);
    private static final String FAKE_PKG = "com.android.shell";

    private static String hex(byte[] b) {
        StringBuilder sb = new StringBuilder(b.length * 2);
        for (byte x : b) sb.append(String.format("%02x", x));
        return sb.toString();
    }

    private static void spoofPackage() {
        try {
            Class<?> atc = Class.forName("android.app.ActivityThread");
            Object at = null;

            Method cur = atc.getDeclaredMethod("currentActivityThread");
            cur.setAccessible(true);
            at = cur.invoke(null);
            System.out.println("currentActivityThread=" + at);

            if (at == null) {
                Method sysMain = atc.getDeclaredMethod("systemMain");
                sysMain.setAccessible(true);
                at = sysMain.invoke(null);
                System.out.println("systemMain -> " + at);
            }
            if (at == null) {
                System.out.println("no ActivityThread, cannot spoof package");
                return;
            }

            Field bf = atc.getDeclaredField("mBoundApplication");
            bf.setAccessible(true);
            Object bind = bf.get(at);
            if (bind == null) {
                Class<?> bindCls = Class.forName("android.app.ActivityThread$AppBindData");
                Constructor<?> c = bindCls.getDeclaredConstructor();
                c.setAccessible(true);
                bind = c.newInstance();
                bf.set(at, bind);
            }

            Field af = bind.getClass().getDeclaredField("appInfo");
            af.setAccessible(true);
            ApplicationInfo ai = (ApplicationInfo) af.get(bind);
            if (ai == null) {
                ai = new ApplicationInfo();
                af.set(bind, ai);
            }
            ai.packageName = FAKE_PKG;
            ai.uid = 0;
            System.out.println("package spoofed -> " + ai.packageName);
        } catch (Throwable t) {
            System.out.println("spoof failed: " + t.getClass().getName() + ": " + t.getMessage());
        }
    }

    public static void main(String[] args) {
        System.out.println("uid=" + android.os.Process.myUid());
        spoofPackage();

        MediaDrm drm = null;
        try {
            drm = new MediaDrm(WIDEVINE);
            try {
                System.out.println("securityLevel=" + drm.getPropertyString("securityLevel"));
            } catch (Throwable t) {
                System.out.println("securityLevel=<" + t.getClass().getSimpleName() + ">");
            }

            byte[] id = drm.getPropertyByteArray(MediaDrm.PROPERTY_DEVICE_UNIQUE_ID);
            System.out.println("DEVICE_UNIQUE_ID=" + hex(id) + " len=" + id.length);

            String[] props = {
                    MediaDrm.PROPERTY_VENDOR,
                    MediaDrm.PROPERTY_VERSION,
                    MediaDrm.PROPERTY_DESCRIPTION,
                    MediaDrm.PROPERTY_ALGORITHMS,
            };
            for (String p : props) {
                try {
                    System.out.println(p + "=" + drm.getPropertyString(p));
                } catch (Throwable t) {
                    System.out.println(p + "=<" + t.getClass().getSimpleName() + ">");
                }
            }
        } catch (MediaDrmException e) {
            System.out.println("MediaDrmException: " + e);
        } catch (Throwable t) {
            System.out.println("FAILED: " + t.getClass().getName() + ": " + t.getMessage());
            Throwable c = t.getCause();
            while (c != null) {
                System.out.println("  caused by " + c.getClass().getName() + ": " + c.getMessage());
                c = c.getCause();
            }
        } finally {
            if (drm != null) {
                try { drm.close(); } catch (Throwable ignored) {}
            }
        }
    }
}