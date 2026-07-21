/* Snapmap+ site — Community section behavior.
   Renders GitHub Discussions (via the community proxy) natively on the site:
   1. Index — category tabs + the post list (community.html).
   2. Post view — one discussion: body, reactions, comment thread (community-post.html).
   Each feature activates only when its markup is present, same as site.js. */

(function () {
  "use strict";

  var WORKER = "https://snapmap-plus-community.doom-snapmap.workers.dev";
  var REPO_URL = "https://github.com/doom-snapmap/snapmap-plus";
  var DISCUSSIONS_URL = REPO_URL + "/discussions";

  function esc(s) {
    return String(s == null ? "" : s)
      .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function api(path) {
    return fetch(WORKER + path)
      .then(function (r) { return r.ok ? r.json() : null; })
      .catch(function () { return null; });
  }

  function fmtDate(iso) {
    var d = new Date(iso);
    var days = (Date.now() - d.getTime()) / 86400000;
    if (days < 1) {
      var hours = Math.floor(days * 24);
      return hours <= 1 ? "just now" : hours + " hours ago";
    }
    if (days < 30) {
      var n = Math.floor(days);
      return n === 1 ? "yesterday" : n + " days ago";
    }
    return d.toLocaleDateString(undefined, { year: "numeric", month: "long", day: "numeric" });
  }

  var REACTION_EMOJI = {
    THUMBS_UP: "👍", THUMBS_DOWN: "👎", LAUGH: "😄",
    HOORAY: "🎉", CONFUSED: "😕", HEART: "❤️",
    ROCKET: "🚀", EYES: "👀"
  };

  function reactionsHtml(reactions) {
    if (!reactions || !reactions.length) return "";
    var html = '<span class="reactions">';
    reactions.forEach(function (r) {
      var emoji = REACTION_EMOJI[r.content];
      if (!emoji) return;
      html += '<span class="reaction-chip">' + emoji + " " + r.count + "</span>";
    });
    return html + "</span>";
  }

  function authorHtml(a) {
    var login = esc(a && a.login || "ghost");
    var avatar = "";
    if (a && a.avatarUrl) {
      var sized = a.avatarUrl + (a.avatarUrl.indexOf("?") >= 0 ? "&" : "?") + "s=48";
      avatar = '<img class="avatar" src="' + esc(sized) + '" alt="" loading="lazy">';
    }
    var name = a && a.url
      ? '<a href="' + esc(a.url) + '" rel="noopener">' + login + "</a>"
      : login;
    return avatar + '<span class="author-name">' + name + "</span>";
  }

  /* ---------- video embeds ----------
     GitHub's rendered HTML leaves video links as plain anchors. Recognize known hosts and
     append a responsive iframe AFTER the link. The embed src is rebuilt from the parsed video
     id — never from the raw href — so only allowlisted forms ever reach an iframe. */

  function videoEmbed(href) {
    var m = href.match(/^https?:\/\/(?:www\.)?youtube\.com\/watch\?(?:.*&)?v=([\w-]{6,20})/) ||
            href.match(/^https?:\/\/youtu\.be\/([\w-]{6,20})/);
    if (m) return "https://www.youtube-nocookie.com/embed/" + m[1];
    m = href.match(/^https?:\/\/(?:www\.)?streamable\.com\/([a-z0-9]{4,12})$/i);
    if (m) return "https://streamable.com/e/" + m[1];
    return null;
  }

  function enhanceVideos(container) {
    var anchors = Array.prototype.slice.call(container.querySelectorAll("a[href]"));
    anchors.forEach(function (a) {
      var src = videoEmbed(a.getAttribute("href") || "");
      if (!src) return;
      /* only bare links (the URL as its own text), not prose links */
      if (a.textContent.trim() !== a.getAttribute("href")) return;
      var frame = document.createElement("div");
      frame.className = "video-embed";
      var iframe = document.createElement("iframe");
      iframe.src = src;
      iframe.setAttribute("allowfullscreen", "");
      iframe.setAttribute("loading", "lazy");
      iframe.setAttribute("title", "Embedded video");
      iframe.setAttribute("allow", "encrypted-media; picture-in-picture");
      frame.appendChild(iframe);
      a.parentNode.insertBefore(frame, a.nextSibling);
    });
  }

  function failNote(host, what) {
    host.innerHTML =
      '<div class="changelog-fallback">Could not load ' + what + " right now. " +
      'The community also lives on <a href="' + DISCUSSIONS_URL + '">GitHub Discussions</a>.</div>';
  }

  /* ---------- 1. index ---------- */

  function initIndex() {
    var list = document.getElementById("community-list");
    if (!list) return;
    var tabs = document.getElementById("community-tabs");
    var more = document.getElementById("community-more");
    var params = new URLSearchParams(location.search);
    var active = params.get("category") || "";

    api("/community/categories").then(function (data) {
      if (!data || !tabs) return;
      var html = '<a class="cat-tab' + (active ? "" : " active") + '" href="community.html">All</a>';
      data.categories.forEach(function (c) {
        html += '<a class="cat-tab' + (c.slug === active ? " active" : "") +
          '" href="community.html?category=' + esc(encodeURIComponent(c.slug)) + '">' +
          esc(c.name) + "</a>";
      });
      tabs.innerHTML = html;
    });

    function renderRows(discussions) {
      var html = "";
      discussions.forEach(function (d) {
        html +=
          '<a class="post-card" href="community-post.html?n=' + d.number + '">' +
            '<div class="post-card-main">' +
              '<h2 class="post-title">' + esc(d.title) + "</h2>" +
              '<div class="post-meta">' +
                authorHtml(d.author) +
                '<span class="sep">&middot;</span><time datetime="' + esc(d.createdAt) + '">' + fmtDate(d.createdAt) + "</time>" +
                (d.category ? '<span class="sep">&middot;</span><span class="cat-pill">' + esc(d.category.name) + "</span>" : "") +
              "</div>" +
            "</div>" +
            '<div class="post-card-counts">' +
              '<span class="count-chip" title="Reactions">&#9650; ' + d.reactionCount + "</span>" +
              '<span class="count-chip" title="Comments">&#128172; ' + d.commentCount + "</span>" +
            "</div>" +
          "</a>";
      });
      return html;
    }

    function load(after, append) {
      var q = "/community/discussions";
      var qs = [];
      if (active) qs.push("category=" + encodeURIComponent(active));
      if (after) qs.push("after=" + encodeURIComponent(after));
      if (qs.length) q += "?" + qs.join("&");
      api(q).then(function (data) {
        if (!data) { if (!append) failNote(list, "community posts"); return; }
        if (!data.discussions.length && !append) {
          list.innerHTML =
            '<div class="changelog-fallback">No posts here yet. Be the first — ' +
            '<a href="' + DISCUSSIONS_URL + '/new/choose">start one on GitHub</a>.</div>';
          if (more) more.innerHTML = "";
          return;
        }
        var html = renderRows(data.discussions);
        if (append) list.insertAdjacentHTML("beforeend", html);
        else list.innerHTML = html;
        if (more) {
          if (data.pageInfo && data.pageInfo.hasNextPage) {
            more.innerHTML = '<button class="btn btn-ghost" type="button">Load more</button>';
            more.querySelector("button").addEventListener("click", function () {
              this.disabled = true;
              load(data.pageInfo.endCursor, true);
            });
          } else {
            more.innerHTML = "";
          }
        }
      });
    }

    load(null, false);
  }

  /* ---------- 2. post view ---------- */

  function initPost() {
    var host = document.getElementById("community-post");
    if (!host) return;
    var params = new URLSearchParams(location.search);
    var n = parseInt(params.get("n") || "", 10);
    if (!n) { location.replace("community.html"); return; }

    api("/community/discussions/" + n).then(function (d) {
      if (!d) { failNote(host, "this post"); return; }
      document.title = d.title + " — Snapmap+ Community";

      var head =
        '<div class="section-head"><h1 class="section-title">' + esc(d.title) + "</h1></div>" +
        '<div class="post-meta post-meta-page">' +
          authorHtml(d.author) +
          '<span class="sep">&middot;</span><time datetime="' + esc(d.createdAt) + '">' + fmtDate(d.createdAt) + "</time>" +
          (d.category ? '<span class="sep">&middot;</span><span class="cat-pill">' + esc(d.category.name) + "</span>" : "") +
        "</div>";

      /* bodyHTML arrives already rendered + sanitized by GitHub's own pipeline */
      host.innerHTML =
        head +
        '<div class="post-body">' + d.bodyHTML + "</div>" +
        '<div class="post-foot">' + reactionsHtml(d.reactions) +
          '<a class="gh-link" href="' + esc(d.url) + '" rel="noopener">React or reply on GitHub &rarr;</a>' +
        "</div>";
      enhanceVideos(host.querySelector(".post-body"));

      var thread = document.getElementById("community-comments");
      if (!thread) return;

      function commentHtml(c, isReply) {
        var html =
          '<article class="comment' + (isReply ? " comment-reply" : "") + '">' +
            '<div class="post-meta">' + authorHtml(c.author) +
              '<span class="sep">&middot;</span><time datetime="' + esc(c.createdAt) + '">' + fmtDate(c.createdAt) + "</time>" +
            "</div>" +
            '<div class="comment-body">' + c.bodyHTML + "</div>" +
            reactionsHtml(c.reactions) +
          "</article>";
        return html;
      }

      var html =
        '<div class="section-head"><h2 class="section-title thread-title">' +
        d.commentCount + (d.commentCount === 1 ? " comment" : " comments") + "</h2></div>";
      if (!d.comments.length) {
        html += '<div class="changelog-fallback">No comments yet. ' +
          '<a href="' + esc(d.url) + '">Reply on GitHub</a> to start the thread.</div>';
      } else {
        d.comments.forEach(function (c) {
          html += commentHtml(c, false);
          c.replies.forEach(function (r) { html += commentHtml(r, true); });
        });
        html += '<div class="post-foot"><a class="gh-link" href="' + esc(d.url) +
          '" rel="noopener">Join the conversation on GitHub &rarr;</a></div>';
      }
      thread.innerHTML = html;
      Array.prototype.slice.call(thread.querySelectorAll(".comment-body")).forEach(enhanceVideos);
    });
  }

  /* ---------- boot ---------- */

  function boot() { initIndex(); initPost(); }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();
