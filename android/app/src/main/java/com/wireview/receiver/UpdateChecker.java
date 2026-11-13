package com.wireview.receiver;

import org.json.JSONObject;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

final class UpdateChecker {
    static final String DOWNLOAD_URL = "https://github.com/liujiaxing060903/WireView/releases/latest";

    static String latestVersion() throws Exception {
        HttpURLConnection connection = (HttpURLConnection) new URL(
                "https://api.github.com/repos/liujiaxing060903/WireView/releases/latest").openConnection();
        connection.setConnectTimeout(8000);
        connection.setReadTimeout(8000);
        connection.setRequestProperty("User-Agent", "WireView/" + BuildConfig.VERSION_NAME);
        connection.setRequestProperty("Accept", "application/vnd.github+json");
        try {
            if (connection.getResponseCode() != 200) throw new java.io.IOException("Update check failed");
            StringBuilder body = new StringBuilder();
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                    connection.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) body.append(line);
            }
            JSONObject release = new JSONObject(body.toString());
            if (release.optBoolean("draft") || release.optBoolean("prerelease")) {
                throw new java.io.IOException("Not a stable release");
            }
            String version = release.getString("tag_name").replaceFirst("^[vV]", "");
            VersionNumber.isNewer(version, BuildConfig.VERSION_NAME);
            return version;
        } finally {
            connection.disconnect();
        }
    }
}
