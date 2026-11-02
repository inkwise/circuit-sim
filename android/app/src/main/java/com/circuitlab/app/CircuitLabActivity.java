package com.circuitlab.app;

import android.os.Bundle;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

/**
 * Thin SDL3 activity. SDLActivity owns the surface, loads libSDL3.so and
 * libmain.so, and calls the exported SDL_main entry point.
 *
 * The rendering loop is vsynced; asking the window for a 120 Hz refresh rate
 * lets supported phones present at 120 fps instead of the default 60 Hz.
 */
public class CircuitLabActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        attributes.preferredRefreshRate = 120.0f;
        getWindow().setAttributes(attributes);
    }

    /**
     * SDL normally locks the requested orientation to the initial window
     * aspect ratio. The manifest uses fullSensor, so leave the system in
     * charge and rebuild the responsive layout after each rotation.
     */
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        // Intentionally empty: the manifest's fullSensor wins.
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL3",
            "main"
        };
    }
}
