package ph.agridose.dashboard;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/*
 * Gives the camera app a place to save a new photo: one file in this app's
 * cache (cache/camera/). The provider is not exported; the camera app only
 * gets a one-time grant to that one file. The dashboard page then shrinks
 * the photo and sends the original and the compressed copy to the machine's
 * SD card, so the copy here is needed only for a moment. Files older than a
 * day are deleted when the app starts.
 */
public class PhotoProvider extends ContentProvider {

    static final String AUTHORITY = "ph.agridose.dashboard.photos";
    private static final String NAME_PATTERN = "agridose_\\d+\\.jpg";

    static Uri newPhotoUri() {
        return new Uri.Builder().scheme("content").authority(AUTHORITY)
                .appendPath("agridose_" + System.currentTimeMillis() + ".jpg").build();
    }

    // True if the camera app really saved a photo there.
    static boolean hasPhoto(Context context, Uri uri) {
        try {
            return fileFor(context, uri).length() > 0;
        } catch (FileNotFoundException e) {
            return false;
        }
    }

    static void deleteOld(Context context) {
        File[] files = folder(context).listFiles();
        if (files == null) return;
        long dayAgo = System.currentTimeMillis() - 24L * 60 * 60 * 1000;
        for (File f : files) {
            if (f.lastModified() < dayAgo) f.delete();
        }
    }

    private static File folder(Context context) {
        File dir = new File(context.getCacheDir(), "camera");
        dir.mkdirs();
        return dir;
    }

    // Only names that newPhotoUri() makes are allowed, so no other file can be reached.
    private static File fileFor(Context context, Uri uri) throws FileNotFoundException {
        String name = uri.getLastPathSegment();
        if (!AUTHORITY.equals(uri.getAuthority()) || uri.getPathSegments().size() != 1
                || name == null || !name.matches(NAME_PATTERN)) {
            throw new FileNotFoundException("Not an AgriDose photo: " + uri);
        }
        return new File(folder(context), name);
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        return ParcelFileDescriptor.open(fileFor(getContext(), uri), ParcelFileDescriptor.parseMode(mode));
    }

    @Override
    public String getType(Uri uri) {
        return "image/jpeg";
    }

    // The file's name and size, which the web view or the camera app may ask for.
    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        File file;
        try {
            file = fileFor(getContext(), uri);
        } catch (FileNotFoundException e) {
            return null;
        }
        String[] columns = projection != null ? projection
                : new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(columns[i])) row[i] = file.getName();
            else if (OpenableColumns.SIZE.equals(columns[i])) row[i] = file.length();
        }
        MatrixCursor cursor = new MatrixCursor(columns, 1);
        cursor.addRow(row);
        return cursor;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("Read-only");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Read-only");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Read-only");
    }
}
