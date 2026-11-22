// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp

/**
 * The "Doors" theme of the unit's own design system (docs/design/themes.json,
 * normal mode): a warm near-black, parchment as the one accent, and the
 * radio's blue and amber kept for receive and transmit. Dark is the system,
 * not a variant.
 */
object Doors {
    val Bg = Color(0xFF0B0B0A)
    val Surface = Color(0xFF161513)
    val Raised = Color(0xFF211F1C)
    val Line = Color(0xFF36332E)
    val Text = Color(0xFFEEEAE2)
    val Secondary = Color(0xFFA8A399)
    val Muted = Color(0xFF89857C)
    val Faint = Color(0xFF5A5752)
    val Accent = Color(0xFFD8BF94)
    val OnAccent = Color(0xFF0B0B0A)
    val Ok = Color(0xFF57C785)
    val Warn = Color(0xFFE3B341)
    val Error = Color(0xFFE5534B)
    val Rx = Color(0xFF8FC1E8)
    val Tx = Color(0xFFEE9960)
}

/**
 * Three voices, all from the phone itself (nothing is fetched): a light serif
 * for names and headlines, a tracked sans for labels, a monospace for every
 * figure the unit reports.
 */
object Type {
    val Display = TextStyle(fontFamily = FontFamily.Serif, fontWeight = FontWeight.Light, fontSize = 34.sp, lineHeight = 40.sp, letterSpacing = (-0.01).em, color = Doors.Text)
    val Title = TextStyle(fontFamily = FontFamily.Serif, fontWeight = FontWeight.Normal, fontSize = 22.sp, lineHeight = 28.sp, color = Doors.Text)
    val Body = TextStyle(fontFamily = FontFamily.SansSerif, fontSize = 15.sp, lineHeight = 22.sp, color = Doors.Secondary)
    val BodyStrong = TextStyle(fontFamily = FontFamily.SansSerif, fontWeight = FontWeight.Medium, fontSize = 15.sp, lineHeight = 22.sp, color = Doors.Text)
    val Label = TextStyle(fontFamily = FontFamily.SansSerif, fontWeight = FontWeight.Medium, fontSize = 11.sp, lineHeight = 14.sp, letterSpacing = 0.16.em, color = Doors.Muted)
    val Figure = TextStyle(fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Normal, fontSize = 26.sp, lineHeight = 30.sp, letterSpacing = (-0.02).em, color = Doors.Text)
    val Mono = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 13.sp, lineHeight = 18.sp, color = Doors.Text)
    /** The one large number of an instrument screen. */
    val Hero = TextStyle(fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Medium, fontSize = 44.sp, lineHeight = 48.sp, letterSpacing = (-0.03).em, color = Doors.Text)
    val Reading = TextStyle(fontFamily = FontFamily.Monospace, fontSize = 20.sp, lineHeight = 24.sp, letterSpacing = (-0.01).em, color = Doors.Text)
    val MonoSmall =TextStyle(fontFamily = FontFamily.Monospace, fontSize = 11.sp, lineHeight = 14.sp, color = Doors.Muted)
    val Button = TextStyle(fontFamily = FontFamily.SansSerif, fontWeight = FontWeight.SemiBold, fontSize = 14.sp, letterSpacing = 0.08.em)
}

@Composable
fun DoorsTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = darkColorScheme(
            primary = Doors.Accent, onPrimary = Doors.OnAccent,
            background = Doors.Bg, onBackground = Doors.Text,
            surface = Doors.Surface, onSurface = Doors.Text,
            surfaceVariant = Doors.Raised, onSurfaceVariant = Doors.Secondary,
            outline = Doors.Line, error = Doors.Error,
        ),
        content = content,
    )
}
