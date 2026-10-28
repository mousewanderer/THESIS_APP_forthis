package ph.agridose.dashboard;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.graphics.Insets;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowInsets;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;
import android.widget.Toast;

import java.io.InputStream;
import java.io.OutputStream;
import java.net.URL;

/*
 * AgriDose dashboard app.
 *
 * A window around the dashboard page that the ESP32 serves at
 * http://192.168.4.1/. Everything on screen comes from the ESP32; this app
 * only does three things a browser cannot:
 *   1. It sends its traffic over the AgriDose Wi-Fi even though that Wi-Fi
 *      has no internet. (Otherwise Android may use mobile data, and
 *      192.168.4.1 cannot be reached that way.)
 *   2. It shows a help page with a Retry button when the ESP32 cannot be
 *      reached, and tries again by itself.
 *   3. It saves the dose-log CSV when "Download CSV" is tapped.
 *
 * Look-only: the app gives the page no extra powers (no JavaScript bridge).
 * Doses are still started only from the machine's keypad.
 */
public class MainActivity extends Activity {

    // Must match AP_IP_TEXT in the firmware's config.h.
    static final String DASHBOARD_URL = "http://192.168.4.1/";
    static final String DASHBOARD_HOST = "192.168.4.1";
    static final String OFFLINE_URL = "file:///android_asset/offline.html";
    static final int SAVE_CSV = 1;

    private WebView web;
    private ConnectivityManager connectivity;
    private ConnectivityManager.NetworkCallback wifiCallback;
    private String pendingCsvUrl;   // the CSV address, while the phone asks where to save it

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (saved != null) pendingCsvUrl = saved.getString("csv");

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(getColor(R.color.brand));   // shows behind the status bar
        web = new WebView(this);
        root.addView(web, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        setContentView(root);

        keepClearOfSystemBars(root);
        setUpWebView();
        useAgriDoseWifi();
        web.loadUrl(DASHBOARD_URL);
    }

    // ---------------------------------------------------------------- page --
    private void setUpWebView() {
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);      // the dashboard is a JavaScript page
        s.setDomStorageEnabled(true);      // it remembers the last tab and farmer
        s.setAllowFileAccess(false);       // the page cannot see files on the phone
        s.setAllowContentAccess(false);

        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                Uri url = request.getUrl();
                if (DASHBOARD_HOST.equals(url.getHost()) || "file".equals(url.getScheme())) {
                    return false;          // the dashboard and the help page stay in the app
                }
                try {
                    startActivity(new Intent(Intent.ACTION_VIEW, url));   // anything else: the browser
                } catch (ActivityNotFoundException ignored) {
                }
                return true;
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                if (request.isForMainFrame()) view.loadUrl(OFFLINE_URL);   // ESP32 not reachable
            }
        });

        web.setDownloadListener((url, userAgent, contentDisposition, mimeType, length) -> askWhereToSaveCsv(url));
    }

    private boolean showingHelpPage() {
        String url = web.getUrl();
        return url == null || url.startsWith("file:");
    }

    // ---------------------------------------------------------- Wi-Fi link --
    // Asks Android for a Wi-Fi network WITHOUT requiring internet, and once
    // there is one, binds this app to it. All of this app's connections
    // (the page, the live feed, the CSV download) then go over Wi-Fi.
    private void useAgriDoseWifi() {
        connectivity = getSystemService(ConnectivityManager.class);
        NetworkRequest wifi = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build();
        wifiCallback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                connectivity.bindProcessToNetwork(network);
                runOnUiThread(() -> {
                    if (showingHelpPage()) web.loadUrl(DASHBOARD_URL);
                });
            }

            @Override
            public void onLost(Network network) {
                connectivity.bindProcessToNetwork(null);
            }
        };
        connectivity.requestNetwork(wifi, wifiCallback);
    }

    // ----------------------------------------------------------- CSV save --
    private void askWhereToSaveCsv(String url) {
        pendingCsvUrl = url;
        Intent pick = new Intent(Intent.ACTION_CREATE_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("text/csv")
                .putExtra(Intent.EXTRA_TITLE, "agridose_dose_log.csv");
        startActivityForResult(pick, SAVE_CSV);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != SAVE_CSV || resultCode != RESULT_OK || data == null || pendingCsvUrl == null) return;
        final Uri target = data.getData();
        final String url = pendingCsvUrl;
        pendingCsvUrl = null;
        new Thread(() -> {                  // network work must not run on the screen's thread
            String message;
            try (InputStream in = new URL(url).openStream();
                 OutputStream out = getContentResolver().openOutputStream(target)) {
                byte[] buffer = new byte[4096];
                int n;
                while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
                message = "Dose log saved";
            } catch (Exception e) {
                message = "Could not save the dose log: " + e.getMessage();
            }
            final String shown = message;
            runOnUiThread(() -> Toast.makeText(this, shown, Toast.LENGTH_LONG).show());
        }).start();
    }

    // -------------------------------------------------------- screen edges --
    // From Android 11 the app draws behind the status bar, navigation bar and
    // keyboard; move the page out from under them (the green root shows there).
    private void keepClearOfSystemBars(View root) {
        if (Build.VERSION.SDK_INT < 30) return;   // older Android places the page below the bars itself
        getWindow().setDecorFitsSystemWindows(false);
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.ime());
            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) web.getLayoutParams();
            lp.setMargins(bars.left, bars.top, bars.right, bars.bottom);
            web.setLayoutParams(lp);
            return WindowInsets.CONSUMED;
        });
    }

    // ------------------------------------------------------------ lifecycle --
    @Override
    public void onBackPressed() {
        if (web.canGoBack()) web.goBack();
        else super.onBackPressed();
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putString("csv", pendingCsvUrl);
    }

    @Override
    protected void onResume() {
        super.onResume();
        web.onResume();
    }

    @Override
    protected void onPause() {
        web.onPause();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (wifiCallback != null) connectivity.unregisterNetworkCallback(wifiCallback);
        connectivity.bindProcessToNetwork(null);
        web.destroy();
        super.onDestroy();
    }
}
