package drmprobe;

import android.media.MediaDrm;
import android.media.MediaDrmException;
import java.lang.reflect.Constructor;
import java.util.UUID;

/**
 * 只做一件事：查一次 Widevine 的 deviceUniqueId，把 32 字节打出来。
 *
 * app_process 环境里 ActivityThread.currentPackageName() 是 null，
 * MediaDrm(UUID) 会抛 "application package name cannot be null"，
 * 所以优先用反射走 MediaDrm(UUID, String) 那个带包名的构造函数。
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

    private static MediaDrm open() throws Exception {
        try {
            return new MediaDrm(WIDEVINE);
        } catch (IllegalArgumentException noPkg) {
            System.out.println("one-arg ctor failed: " + noPkg.getMessage());
        }
        for (Constructor<?> c : MediaDrm.class.getDeclaredConstructors()) {
            System.out.println("ctor: " + c);
        }
        Constructor<MediaDrm> c2 =
                MediaDrm.class.getDeclaredConstructor(UUID.class, String.class);
        c2.setAccessible(true);
        return c2.newInstance(WIDEVINE, FAKE_PKG);
    }

    public static void main(String[] args) {
        System.out.println("uid=" + android.os.Process.myUid());
        MediaDrm drm = null;
        try {
            drm = open();
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