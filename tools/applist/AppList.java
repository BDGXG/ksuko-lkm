package applist;

import android.content.pm.ApplicationInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.util.DisplayMetrics;

import java.io.File;
import java.io.FileOutputStream;
import java.io.PrintStream;
import java.lang.reflect.Method;
import java.util.List;

/**
 * 用系统 PackageManager 枚举应用，并让框架自己把图标渲染成 PNG。
 * 不解析 APK、不猜图层，自适应图标/圆角/蒙版全部交给系统。
 *
 * 运行：
 *   CLASSPATH=/data/adb/modules/dim/bin/applist.jar \
 *   app_process /system/bin applist.AppList /data/adb/modules/dim/webroot/icons
 *
 * 输出（stdout，每行一条）：pkg \t uid \t label \t 1|0(是否有图标)
 */
public final class AppList {
    private static final int SIZE = 144;

    public static void main(String[] args) {
        String outDir = args.length > 0 ? args[0] : "/data/adb/modules/dim/webroot/icons";
        PrintStream out = System.out;
        try {
            new File(outDir).mkdirs();
        } catch (Throwable t) { /* ignore */ }

        try {
            Object pm = getPackageManager();
            if (pm == null) { out.println("ERR\tno-pm\t-\t0"); return; }

            List<?> apps = getInstalledApplications(pm);
            if (apps == null) { out.println("ERR\tno-apps\t-\t0"); return; }

            for (Object o : apps) {
                if (!(o instanceof ApplicationInfo)) continue;
                ApplicationInfo ai = (ApplicationInfo) o;

                // 只要用户安装的应用（/data/app 下，且非系统应用）
                if (ai.sourceDir == null || !ai.sourceDir.startsWith("/data/app")) continue;
                if ((ai.flags & ApplicationInfo.FLAG_SYSTEM) != 0) continue;

                String pkg = ai.packageName;
                String label = pkg;
                int ok = 0;
                try {
                    Resources res = resourcesFor(ai.sourceDir);
                    if (res != null) {
                        try {
                            CharSequence cs = res.getText(ai.labelRes);
                            if (cs != null && cs.length() > 0) label = cs.toString();
                        } catch (Throwable ignore) { }
                        int iconId = ai.icon != 0 ? ai.icon : ai.roundIcon;
                        if (iconId != 0) ok = renderIcon(res, iconId, new File(outDir, pkg + ".png")) ? 1 : 0;
                    }
                } catch (Throwable ignore) { }

                out.println(pkg + "\t" + ai.uid + "\t" + label.replace('\t', ' ').replace('\n', ' ') + "\t" + ok);
            }
        } catch (Throwable t) {
            out.println("ERR\t" + t.getClass().getSimpleName() + "\t-\t0");
        }
        out.flush();
    }

    private static Object getPackageManager() {
        try {
            Class<?> ag = Class.forName("android.app.AppGlobals");
            Method m = ag.getDeclaredMethod("getPackageManager");
            m.setAccessible(true);
            return m.invoke(null);
        } catch (Throwable t) {
            return null;
        }
    }

    private static List<?> getInstalledApplications(Object pm) {
        for (Method m : pm.getClass().getMethods()) {
            if (!m.getName().equals("getInstalledApplications")) continue;
            Class<?>[] p = m.getParameterTypes();
            Object[] argv;
            if (p.length == 2 && p[0] == long.class) argv = new Object[]{ 0L, 0 };
            else if (p.length == 2 && p[0] == int.class) argv = new Object[]{ 0, 0 };
            else if (p.length == 1) argv = new Object[]{ 0 };
            else continue;
            try {
                Object r = m.invoke(pm, argv);
                if (r instanceof List) return (List<?>) r;
            } catch (Throwable ignore) { }
        }
        return null;
    }

    private static Resources resourcesFor(String apkPath) {
        try {
            Class<?> amc = Class.forName("android.content.res.AssetManager");
            AssetManager am = (AssetManager) amc.getDeclaredConstructor().newInstance();
            Method add = amc.getMethod("addAssetPath", String.class);
            add.invoke(am, apkPath);

            DisplayMetrics dm = new DisplayMetrics();
            dm.setToDefaults();
            Configuration cfg = new Configuration();
            cfg.setToDefaults();
            return new Resources(am, dm, cfg);
        } catch (Throwable t) {
            return null;
        }
    }

    private static boolean renderIcon(Resources res, int iconId, File dst) {
        try {
            Drawable d = res.getDrawable(iconId, null);
            if (d == null) return false;
            Bitmap bmp = Bitmap.createBitmap(SIZE, SIZE, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(bmp);
            c.drawColor(0x00000000);
            d.setBounds(0, 0, SIZE, SIZE);
            d.draw(c);
            FileOutputStream fos = new FileOutputStream(dst);
            boolean ok = bmp.compress(Bitmap.CompressFormat.PNG, 100, fos);
            fos.close();
            bmp.recycle();
            return ok && dst.length() > 200;
        } catch (Throwable t) {
            return false;
        }
    }
}