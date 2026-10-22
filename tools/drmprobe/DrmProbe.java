package drmprobe;

import android.media.MediaDrm;
import android.media.MediaDrmException;
import java.util.UUID;

/**
 * 只做一件事：查一次 Widevine 的 deviceUniqueId，把 32 字节打出来。
 * 用于给 drm_guard 内核探针制造一个确定的触发点。
 *
 * 运行方式（root）：
 *   CLASSPATH=/data/local/tmp/drmprobe.jar app_process /system/bin drmprobe.DrmProbe
 */
public final class DrmProbe {
    private static final UUID WIDEVINE =
            new UUID(0xedef8ba979d64aceL, 0xa3c827dcd51d21edL);

    private static String hex(byte[] b) {
        StringBuilder sb = new StringBuilder(b.length * 2);
        for (byte x : b) sb.append(String.format("%02x", x));
        return sb.toString();
    }

    public static void main(String[] args) {
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
        } finally {
            if (drm != null) {
                try { drm.close(); } catch (Throwable ignored) {}
            }
        }
    }
}