package com.phonecam

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/**
 * Lets the PC drive the running stream over adb without foregrounding the app — currently just the
 * camera flip (front/back). The desktop app broadcasts to this receiver; we forward the action to the
 * already-running foreground [StreamService]. Exported so `adb shell am broadcast` can reach it.
 */
class ControlReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == StreamService.ACTION_SWITCH_CAMERA) {
            // StreamService is already a running foreground service, so delivering an intent to it is fine.
            context.startService(Intent(context, StreamService::class.java).apply {
                action = StreamService.ACTION_SWITCH_CAMERA
            })
        }
    }
}
