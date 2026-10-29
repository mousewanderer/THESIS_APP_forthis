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
import android.provider.MediaStore;
import android.view.View;
import android.view.WindowInsets;
import android.webkit.URLUtil;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
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
 * only does what a browser cannot, or what a web view does not do by itself:
 *   1. It sends its traffic over the AgriDose Wi-Fi even though that Wi-Fi
 *      has no internet. (Otherwise Android may use mobile data, and
 *      192.168.4.1 cannot be reached that way.)
 *   2. It shows a help page with a Retry button when the ESP32 cannot be
 *      reached, and tries again by itself.
 *   3. It saves downloads (the dose-log CSV, crop photos) where the farmer
 *      chooses.
 *   4. It answers the page's photo buttons: "Take photo" opens the camera,
 *      "Choose photo" the gallery. The page then does the heavy work
 *      (shrinking the photo, the recommendation); the ESP32 only stores.
 *
 * Look-only: the app gives the page no extra powers (no JavaScript bridge).
 * Doses are still started only from the machine's keypad.
 */
public class MainActivity extends Activity {

    // Must match AP_IP_TEXT in the firmware's config.h.
    static final String DASHBOARD_URL = "http://192.168.4.1/";
    static final String DASHBOARD_HOST = "192.168.4.1";
    static final String OFFLINE_URL = "file:///android_asset/offline.html";
    static final int SAVE_FILE = 1;
    static final int PICK_PHOTO = 2;

    private WebView web;
    private ConnectivityManager connectivity;
    private ConnectivityManager.NetworkCallback wifiCallback;
    private String pendingDownloadUrl;          // the file's address, while the phone asks where to save it
    private ValueCallback<Uri[]> photoCallback; // the page, waiting for a photo
    private Uri cameraUri;                      // where the camera app saves a new photo

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (saved != null) pendingDownloadUrl = saved.getString("download");
        PhotoProvider.deleteOld(this);

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
        s.setAllowFileAccess(false);       // the page cannot open files on the phone by address
        s.setAllowContentAccess(true);     // the photo buttons hand it a content:// link to the one chosen photo

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

        web.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onShowFileChooser(WebView view, ValueCallback<Uri[]> callback, FileChooserParams params) {
                return pickPhoto(callback, params);
            }
        });

        web.setDownloadListener((url, userAgent, contentDisposition, mimeType, length) ->
                askWhereToSave(url, URLUtil.guessFileName(url, contentDisposition, mimeType), mimeType));
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

    // -------------------------------------------------------------- photos --
    // "Take photo" on the page has capture="environment": open the camera.
    // "Choose photo" has not: open the gallery or file picker.
    private boolean pickPhoto(ValueCallback<Uri[]> callback, WebChromeClient.FileChooserParams params) {
        if (photoCallback != null) photoCallback.onReceiveValue(null);   // an earlier picker never answered
        photoCallback = callback;
        cameraUri = null;
        Intent intent;
        if (params.isCaptureEnabled()) {
            cameraUri = PhotoProvider.newPhotoUri();
            intent = new Intent(MediaStore.ACTION_IMAGE_CAPTURE)
                    .putExtra(MediaStore.EXTRA_OUTPUT, cameraUri)
                    .addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } else {
            intent = params.createIntent();
        }
        try {
            startActivityForResult(intent, PICK_PHOTO);
        } catch (ActivityNotFoundException e) {
            Toast.makeText(this, cameraUri != null ? "No camera app found" : "No photo app found", Toast.LENGTH_LONG).show();
            photoCallback.onReceiveValue(null);
            photoCallback = null;
            cameraUri = null;
        }
        return true;
    }

    // ----------------------------------------------------------- downloads --
    private void askWhereToSave(String url, String name, String mimeType) {
        pendingDownloadUrl = url;
        Intent pick = new Intent(Intent.ACTION_CREATE_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType(mimeType == null || mimeType.isEmpty() ? "application/octet-stream" : mimeType)
                .putExtra(Intent.EXTRA_TITLE, name);
        try {
            startActivityForResult(pick, SAVE_FILE);
        } catch (ActivityNotFoundException e) {
            pendingDownloadUrl = null;
            Toast.makeText(this, "This phone has no app to save files", Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == PICK_PHOTO) {
            if (photoCallback == null) return;
            Uri[] result = null;               // null = cancelled; the page must always get an answer
            if (resultCode == RESULT_OK) {
                if (cameraUri != null) {
                    if (PhotoProvider.hasPhoto(this, cameraUri)) result = new Uri[] {cameraUri};
                } else {
                    result = WebChromeClient.FileChooserParams.parseResult(resultCode, data);
                }
            }
            photoCallback.onReceiveValue(result);
            photoCallback = null;
            cameraUri = null;
            return;
        }
        if (requestCode != SAVE_FILE || resultCode != RESULT_OK || data == null || pendingDownloadUrl == null) return;
        final Uri target = data.getData();
        final String url = pendingDownloadUrl;
        pendingDownloadUrl = null;
        new Thread(() -> {                  // network work must not run on the screen's thread
            String message;
            try (InputStream in = new URL(url).openStream();
                 OutputStream out = getContentResolver().openOutputStream(target)) {
                byte[] buffer = new byte[4096];
                int n;
                while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
                message = "Saved";
            } catch (Exception e) {
                message = "Could not save the file: " + e.getMessage();
            }
            final String shown = message;
            runOnUiThread(() -> Toast.makeText(this, shown, Toast.LENGTH_LONG).show());
        }).start();
    }

    // -------------------------------------------------------- screen edges --
    // From Android 11 the app draws behind the status bar, navigation bar and
    // keyboard; move the page out from under them (the dark root shows there).
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
        out.putString("download", pendingDownloadUrl);
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
