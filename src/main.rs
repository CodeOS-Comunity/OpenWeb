use axum::{
    extract::Query,
    http::StatusCode,
    response::{Html, IntoResponse, Response},
    routing::{get, post},
    Json, Router,
};
use serde::{Deserialize, Serialize};
use std::{collections::HashMap, process::Command};
use tokio::net::TcpListener;
use tower_http::services::ServeDir;

#[derive(Clone)]
struct AppState {}

#[derive(Debug, Deserialize)]
struct SearchRequest {
    query: String,
    engine: Option<String>,
}

#[derive(Debug, Serialize)]
struct SearchResponse {
    engine: String,
    query: String,
    url: String,
    mode: String,
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let root = std::env::current_dir()?;
    let state = AppState {};
    let static_dir = root.join("static");

    let app = Router::new()
        .route("/", get(index_handler))
        .route("/search", post(search_handler))
        .route("/view", get(view_handler))
        .route("/render", get(render_handler))
        .nest_service("/static", ServeDir::new(&static_dir))
        .fallback_service(ServeDir::new(&static_dir))
        .with_state(state);

    let listener = TcpListener::bind("0.0.0.0:3000").await?;
    println!("OpenWeb listening on http://127.0.0.1:3000");
    axum::serve(listener, app).await?;
    Ok(())
}

async fn index_handler() -> impl IntoResponse {
    let file = std::env::current_dir().unwrap().join("static").join("index.html");
    match tokio::fs::read_to_string(file).await {
        Ok(content) => Html(content).into_response(),
        Err(err) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("Unable to load browser shell: {err}"),
        )
            .into_response(),
    }
}

async fn view_handler(Query(params): Query<HashMap<String, String>>) -> impl IntoResponse {
    let target = params.get("target").cloned().unwrap_or_else(|| "https://codeos.dev".to_string());
    let normalized = normalize_url(&target);
    let page = build_view_page(&normalized);
    Html(page).into_response()
}

async fn render_handler(Query(params): Query<HashMap<String, String>>) -> impl IntoResponse {
    let target = params.get("target").cloned().unwrap_or_else(|| "https://codeos.dev".to_string());
    let normalized = normalize_url(&target);
    match fetch_and_render(&normalized).await {
        Ok(page) => Html(page).into_response(),
        Err(err) => {
            let page = format!(
                r#"<!DOCTYPE html>
<html lang="en">
  <head><meta charset="utf-8" /><title>OpenWeb render error</title>
  <style>body{{font-family:Inter,Arial,sans-serif;padding:24px;background:#0f172a;color:#e2e8f0;}} .card{{border:1px solid #334155;border-radius:16px;padding:20px;background:#111827;max-width:720px;}} code{{background:#1e293b;padding:2px 6px;border-radius:6px;}}</style></head>
  <body><div class="card"><h1>Secure page could not be rendered</h1><p><code>{}</code></p><p>The page may be blocked by the network or may not allow remote fetches.</p></div></body></html>"#,
                escape_html(&err)
            );
            Html(page).into_response()
        }
    }
}

async fn search_handler(Json(payload): Json<SearchRequest>) -> Response {
    let query = payload.query.trim();
    let engine = payload.engine.as_deref().unwrap_or("google");

    let (selected_engine, url, mode) = if looks_like_url(query) {
        (
            "https",
            normalize_url(query),
            "direct",
        )
    } else {
        let escaped = query.replace(' ', "+");
        if engine == "ddg" {
            (
                "duckduckgo",
                format!("https://duckduckgo.com/?q={escaped}"),
                "search",
            )
        } else {
            (
                "google",
                format!("https://www.google.com/search?q={escaped}"),
                "search",
            )
        }
    };

    let response = SearchResponse {
        engine: selected_engine.to_string(),
        query: query.to_string(),
        url,
        mode: mode.to_string(),
    };

    Json(response).into_response()
}

fn normalize_url(input: &str) -> String {
    let trimmed = input.trim();
    if trimmed.starts_with("http://") || trimmed.starts_with("https://") {
        trimmed.to_string()
    } else if trimmed.starts_with("www.") {
        format!("https://{trimmed}")
    } else {
        format!("https://{trimmed}")
    }
}

fn looks_like_url(input: &str) -> bool {
    input.contains("://")
        || input.starts_with("localhost")
        || input.starts_with("127.")
        || input.starts_with("0.0.0.0")
        || (input.contains('.') && !input.contains(' '))
}

fn build_view_page(target: &str) -> String {
    let escaped_target = escape_html(target);
    format!(
        r#"<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>OpenWeb secure view</title>
    <style>
      :root {{ color-scheme: dark; }}
      body {{ margin: 0; font-family: Inter, Arial, sans-serif; background: linear-gradient(135deg, #020617, #111827); color: #e2e8f0; }}
      .shell {{ padding: 24px; }}
      .card {{ border: 1px solid #334155; background: rgba(17,24,39,.95); border-radius: 16px; padding: 20px; box-shadow: 0 20px 50px rgba(0,0,0,.25); max-width: 760px; }}
      .pill {{ display: inline-block; padding: 6px 10px; border-radius: 999px; background: #2563eb; color: white; margin-bottom: 12px; font-size: 0.9rem; }}
      a {{ color: #93c5fd; }}
      code {{ background: #1e293b; padding: 2px 6px; border-radius: 6px; }}
    </style>
  </head>
  <body>
    <div class="shell">
      <div class="card">
        <div class="pill">HTTPS secure view</div>
        <h1>OpenWeb tab content</h1>
        <p>The browser is navigating to:</p>
        <p><code>{escaped_target}</code></p>
        <p>The rendering surface now uses a lightweight tabbed view with HTTPS-aware navigation and a backend renderer.</p>
        <p><a href="{escaped_target}" target="_blank" rel="noreferrer">Open externally</a></p>
      </div>
    </div>
  </body>
</html>"#
    )
}

async fn fetch_and_render(target: &str) -> Result<String, String> {
    let output = Command::new("curl")
        .arg("-L")
        .arg("-A")
        .arg("OpenWeb/0.1")
        .arg(target)
        .output()
        .map_err(|err| format!("curl failed: {err}"))?;

    if !output.status.success() {
        return Err(format!("curl exited with status {}", output.status));
    }

    let body = String::from_utf8(output.stdout).map_err(|err| format!("invalid utf-8: {err}"))?;
    let status = "200 OK".to_string();
    let title = extract_title(&body);
    let preview = clean_text(&body);
    let escaped_title = escape_html(&title);
    let escaped_preview = escape_html(&preview);
    let escaped_target = escape_html(target);
    let escaped_status = escape_html(&status);

    Ok(format!(
        r#"<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>{escaped_title}</title>
    <style>
      :root {{ color-scheme: dark; }}
      body {{ margin: 0; font-family: Inter, Arial, sans-serif; background: #f8fafc; color: #0f172a; }}
      .shell {{ padding: 24px; }}
      .card {{ border: 1px solid #cbd5e1; background: white; border-radius: 16px; padding: 24px; box-shadow: 0 20px 50px rgba(15, 23, 42, 0.12); max-width: 860px; }}
      .pill {{ display: inline-block; padding: 6px 10px; border-radius: 999px; background: #2563eb; color: white; margin-bottom: 14px; font-size: 0.9rem; }}
      .meta {{ color: #475569; margin-bottom: 12px; }}
      a {{ color: #2563eb; }}
      code {{ background: #e2e8f0; padding: 2px 6px; border-radius: 6px; }}
      p {{ line-height: 1.6; }}
    </style>
  </head>
  <body>
    <div class="shell">
      <div class="card">
        <div class="pill">OpenWeb renderer</div>
        <h1>{escaped_title}</h1>
        <div class="meta">{escaped_status} • {escaped_target}</div>
        <p>{escaped_preview}</p>
      </div>
    </div>
  </body>
</html>"#
    ))
}

fn extract_title(html: &str) -> String {
    let lower = html.to_lowercase();
    if let Some(start) = lower.find("<title>") {
        let title_start = start + "<title>".len();
        if let Some(end) = lower[title_start..].find("</title>") {
            return html[title_start..title_start + end].trim().to_string();
        }
    }
    "Secure page".to_string()
}

fn clean_text(html: &str) -> String {
    let without_tags = strip_tags(html);
    let compact = without_tags
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ");
    compact.chars().take(1100).collect::<String>()
}

fn strip_tags(html: &str) -> String {
    let mut output = String::new();
    let mut in_tag = false;
    for ch in html.chars() {
        match ch {
            '<' => in_tag = true,
            '>' => in_tag = false,
            _ if !in_tag => output.push(ch),
            _ => {}
        }
    }
    output
}

fn escape_html(input: &str) -> String {
    input
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&#39;")
}
