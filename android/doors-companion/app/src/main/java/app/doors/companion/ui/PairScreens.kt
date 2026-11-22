// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.ui

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.togetherWith
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.LocalLifecycleOwner
import app.doors.companion.Link
import app.doors.companion.core.Fingerprint
import app.doors.companion.core.PairOutcome
import app.doors.companion.core.PairingUri
import com.google.zxing.BinaryBitmap
import com.google.zxing.PlanarYUVLuminanceSource
import com.google.zxing.ReaderException
import com.google.zxing.common.HybridBinarizer
import com.google.zxing.qrcode.QRCodeReader
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.concurrent.Executors

private sealed interface Step {
    data object Welcome : Step
    data object Scan : Step
    data object Paste : Step
    class Confirm(val code: PairingUri) : Step
    class Waiting(val code: PairingUri) : Step
    class Failed(val title: String, val text: String) : Step
}

/**
 * Pairing (PAIRING.md section 3). The code comes from the unit's own screen;
 * the unit's name and key are shown before anything is stored or sent; the
 * decision is a tap on the unit.
 */
@Composable
fun PairFlow(link: Link) {
    var step by remember { mutableStateOf<Step>(Step.Welcome) }
    var tokenTaken by remember { mutableStateOf(false) }
    var job by remember { mutableStateOf<Job?>(null) }
    val scope = rememberCoroutineScope()
    val mine by produceState<String?>(null) {
        value = withContext(Dispatchers.IO) {
            try {
                Fingerprint.display(link.identity.fingerprint())
            } catch (e: Exception) {
                null
            }
        }
    }

    fun accept(text: String): String? = try {
        step = Step.Confirm(PairingUri.parse(text))
        null
    } catch (e: PairingUri.Invalid) {
        e.message
    }

    BackHandler(enabled = step != Step.Welcome) {
        job?.cancel()
        step = Step.Welcome
    }

    AnimatedContent(
        step, label = "pair",
        transitionSpec = { (fadeIn(tween(280)) + slideInVertically(tween(280)) { it / 14 }) togetherWith fadeOut(tween(160)) },
    ) { s ->
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).statusBarsPadding().navigationBarsPadding().imePadding()
                .padding(horizontal = 24.dp, vertical = 16.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            when (s) {
                Step.Welcome -> {
                    Spacer(Modifier.height(8.dp))
                    Label("Doors  ·  Companion", color = Doors.Accent)
                    Threshold(0.07f, Doors.Accent, Modifier.fillMaxWidth().height(250.dp), searching = true)
                    Text("A door to\nyour unit.", style = Type.Display)
                    Text(
                        "On the unit, open Settings, Companion, Pair a phone. It shows a code for two minutes. The code carries the unit's key, so this phone knows the unit before it says anything.",
                        style = Type.Body,
                    )
                    Spacer(Modifier.height(6.dp))
                    PrimaryButton("Scan the code") { step = Step.Scan }
                    GhostButton("Paste a pairing link") { step = Step.Paste }
                    Spacer(Modifier.height(6.dp))
                    Label("This phone's key")
                    Text(mine ?: "····  ····  ····  ····", style = Type.Mono.copy(color = Doors.Secondary))
                }

                Step.Scan -> {
                    TopBar("Scan the code") { step = Step.Welcome }
                    var error by remember { mutableStateOf<String?>(null) }
                    Scanner(Modifier.fillMaxWidth().aspectRatio(1f)) { text -> error = accept(text) }
                    Text(error ?: "Hold the unit's screen inside the frame.", style = Type.Body, color = if (error != null) Doors.Warn else Doors.Secondary)
                    GhostButton("Paste a link instead") { step = Step.Paste }
                }

                Step.Paste -> {
                    TopBar("Pairing link") { step = Step.Welcome }
                    var text by remember { mutableStateOf("") }
                    var error by remember { mutableStateOf<String?>(null) }
                    Text("Paste the doors-pair link the unit printed. It holds a one-time token: paste it nowhere else.", style = Type.Body)
                    BasicTextField(
                        text, { text = it; error = null },
                        textStyle = Type.Mono, cursorBrush = SolidColor(Doors.Accent), minLines = 4,
                        modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(Doors.Surface)
                            .border(1.dp, if (error != null) Doors.Warn else Doors.Line, RoundedCornerShape(16.dp)).padding(16.dp),
                    )
                    error?.let { Text(it.replaceFirstChar { c -> c.uppercase() }, style = Type.Body, color = Doors.Warn) }
                    PrimaryButton("Continue", enabled = text.isNotBlank()) { error = accept(text) }
                }

                is Step.Confirm -> {
                    TopBar("Is this your unit?") { step = Step.Welcome }
                    Panel(Modifier.fillMaxWidth()) {
                        Label("Unit")
                        Text(s.code.name, style = Type.Display, modifier = Modifier.padding(top = 4.dp, bottom = 16.dp))
                        Label("Its key")
                        Text(Fingerprint.display(s.code.fingerprint), style = Type.Figure.copy(color = Doors.Accent), modifier = Modifier.padding(top = 6.dp, bottom = 16.dp))
                        Hairline()
                        KeyValue("Address", "${s.code.host}:${s.code.port}")
                    }
                    Text("From now on the app talks only to the holder of this key. If the digits differ from the unit's screen, stop here.", style = Type.Body)
                    PrimaryButton("Pair with this unit") {
                        tokenTaken = false
                        step = Step.Waiting(s.code)
                        job = scope.launch {
                            when (val out = link.pair(s.code) { tokenTaken = true }) {
                                is PairOutcome.Paired -> Unit // the unit is stored: the app moves to its dashboard
                                PairOutcome.Refused -> step = Step.Failed("Not let in", "The unit refused, the code had expired, or nobody answered in time. Open a new code on the unit and try again.")
                                PairOutcome.WrongUnit -> step = Step.Failed("Not that unit", "The address answered with a different key than the code promised. Nothing was sent to it.")
                                is PairOutcome.Failed -> step = Step.Failed("No answer", out.reason.replaceFirstChar { it.uppercase() } + ". Check that this phone is on the unit's Wi-Fi network.")
                            }
                        }
                    }
                    GhostButton("Cancel") { step = Step.Welcome }
                }

                is Step.Waiting -> {
                    Spacer(Modifier.height(8.dp))
                    Label(if (tokenTaken) "Waiting for the unit" else "Reaching the unit", color = Doors.Accent)
                    Threshold(if (tokenTaken) 0.22f else 0.05f, Doors.Accent, Modifier.fillMaxWidth().height(250.dp), searching = true)
                    Text(if (tokenTaken) "Allow it\non the unit." else "Knocking.", style = Type.Display)
                    Text("The unit shows this phone's name and key. Check the digits, then tap Allow there.", style = Type.Body)
                    Panel(Modifier.fillMaxWidth()) {
                        Label("This phone's key")
                        Text(mine ?: "····  ····  ····  ····", style = Type.Figure, modifier = Modifier.padding(top = 6.dp))
                    }
                    GhostButton("Cancel") {
                        job?.cancel()
                        step = Step.Welcome
                    }
                }

                is Step.Failed -> {
                    Spacer(Modifier.height(8.dp))
                    Label("Pairing", color = Doors.Warn)
                    Threshold(0f, Doors.Warn, Modifier.fillMaxWidth().height(250.dp))
                    Text(s.title, style = Type.Display)
                    Text(s.text, style = Type.Body)
                    PrimaryButton("Start again") { step = Step.Welcome }
                }
            }
        }
    }
}

@Composable
private fun Scanner(modifier: Modifier, onCode: (String) -> Unit) {
    val context = LocalContext.current
    val owner = LocalLifecycleOwner.current
    var granted by remember { mutableStateOf(ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) }
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted = it }
    val shape = RoundedCornerShape(24.dp)
    if (!granted) {
        Column(modifier.clip(shape).background(Doors.Surface).border(1.dp, Doors.Line, shape).padding(24.dp), verticalArrangement = Arrangement.Center) {
            Text("The camera reads the unit's code and nothing else. No picture is kept.", style = Type.Body)
            Spacer(Modifier.height(16.dp))
            PrimaryButton("Allow the camera") { ask.launch(Manifest.permission.CAMERA) }
        }
        return
    }
    val executor = remember { Executors.newSingleThreadExecutor() }
    DisposableEffect(Unit) {
        onDispose {
            try {
                ProcessCameraProvider.getInstance(context).get().unbindAll()
            } catch (_: Exception) {
            }
            executor.shutdown()
        }
    }
    Box(modifier.clip(shape).border(1.dp, Doors.Accent.copy(alpha = 0.6f), shape)) {
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { c ->
                PreviewView(c).also { view ->
                    val future = ProcessCameraProvider.getInstance(c)
                    future.addListener({
                        val provider = future.get()
                        val preview = Preview.Builder().build()
                        preview.setSurfaceProvider(view.surfaceProvider)
                        val analysis = ImageAnalysis.Builder().setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST).build()
                        var last = ""
                        analysis.setAnalyzer(executor) { image ->
                            val text = image.use { decode(it) }
                            if (text != null && text != last) {
                                last = text
                                view.post { onCode(text) }
                            }
                        }
                        try {
                            provider.unbindAll()
                            provider.bindToLifecycle(owner, CameraSelector.DEFAULT_BACK_CAMERA, preview, analysis)
                        } catch (_: Exception) {
                        }
                    }, ContextCompat.getMainExecutor(c))
                }
            },
        )
    }
}

/** The luminance plane through a QR reader. Nothing but the decoded text leaves this function. */
private fun decode(image: ImageProxy): String? {
    val plane = image.planes[0]
    val w = image.width
    val h = image.height
    val data = ByteArray(w * h)
    val buf = plane.buffer
    for (row in 0 until h) {
        buf.position(row * plane.rowStride)
        buf.get(data, row * w, minOf(w, buf.remaining()))
    }
    return try {
        QRCodeReader().decode(BinaryBitmap(HybridBinarizer(PlanarYUVLuminanceSource(data, w, h, 0, 0, w, h, false)))).text
    } catch (e: ReaderException) {
        null
    }
}
