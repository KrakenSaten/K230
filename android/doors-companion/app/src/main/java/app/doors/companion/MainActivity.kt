// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.doors.companion.ui.ConnectionScreen
import app.doors.companion.ui.Doors
import app.doors.companion.ui.DoorsTheme
import app.doors.companion.ui.Home
import app.doors.companion.ui.PacketSharkScreen
import app.doors.companion.ui.PairFlow
import app.doors.companion.ui.RiftScreen
import app.doors.companion.ui.Screen
import app.doors.companion.ui.TelemetryScreen

class MainActivity : ComponentActivity() {
    private val link get() = (application as DoorsApp).link

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val bars = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT)
        enableEdgeToEdge(statusBarStyle = bars, navigationBarStyle = bars)
        setContent { DoorsTheme { Root(link) } }
    }

    // The connection lives exactly as long as the app is in front.
    override fun onStart() {
        super.onStart()
        link.onForeground()
    }

    override fun onStop() {
        link.onBackground()
        super.onStop()
    }
}

@Composable
private fun Root(link: Link) {
    val unit by link.store.unit.collectAsStateWithLifecycle()
    var screen by rememberSaveable { mutableStateOf(Screen.Home) }
    LaunchedEffect(unit == null) { if (unit == null) screen = Screen.Home }
    BackHandler(enabled = unit != null && screen != Screen.Home) { screen = Screen.Home }

    Box(
        Modifier.fillMaxSize().background(Doors.Bg).drawBehind {
            // A little warmth at the top of every page, as if from a lamp above the door.
            drawRect(
                Brush.radialGradient(
                    listOf(Doors.Accent.copy(alpha = 0.07f), Color.Transparent),
                    center = Offset(size.width * 0.5f, -size.width * 0.15f), radius = size.width * 1.1f,
                ),
            )
        },
    ) {
        // null is the pairing flow; otherwise the screen of the paired unit.
        val target: Screen? = if (unit == null) null else screen
        AnimatedContent(
            target, label = "nav",
            transitionSpec = {
                val forward = targetState != Screen.Home && targetState != null
                val dir = if (forward) 1 else -1
                (fadeIn(tween(260)) + slideInHorizontally(tween(320)) { dir * it / 7 }) togetherWith
                    (fadeOut(tween(140)) + slideOutHorizontally(tween(320)) { -dir * it / 7 })
            },
        ) { s ->
            val u = unit
            val back = { screen = Screen.Home }
            when {
                s == null || u == null -> PairFlow(link)
                s == Screen.Home -> Home(link, u) { screen = it }
                s == Screen.Telemetry -> TelemetryScreen(link, back)
                s == Screen.Connection -> ConnectionScreen(link, u, back)
                s == Screen.Rift -> RiftScreen(link, u, back)
                else -> PacketSharkScreen(link, back)
            }
        }
    }
}
