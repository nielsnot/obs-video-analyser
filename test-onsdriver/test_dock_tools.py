'''
Dock tools of 0.9.0: the hotkeys that pause all scopes, switch views and take a snapshot.
They are triggered through obs-websocket the way a Stream Deck does.
'''

import base64
import io
import json
import os
import time
import unittest
from onsdriver import obstest

DOCK_KEY = 'obs-video-analyser-dock'
PREFIX = 'obs-video-analyser.'


def notice(msg):
    'Show a line in the GitHub Actions summary.'
    print(f'::notice::{msg}', flush=True)


def scope(kind, label, prop=None):
    'One scope entry following the dock selection.'
    return {'type': kind, 'label': label, 'show_label': True, 'use_roi': True, 'target': '',
            'prop': dict({'target_scale': 2}, **(prop or {}))}


def dock_data():
    'A dock with two views: a waveform, and a histogram.'
    wv = scope('waveform', 'WV', {'components': 0x20, 'display': 0, 'intensity': 255, 'graticule_lines': 0,
                                  'scale_labels': 0, 'minmax': False, 'out_highlight': False, 'hdr_ref': False,
                                  'ext_guides': False})
    wv['show_label'] = False
    return {
        'docks': [{
            'name': 'Tools',
            'toolbar': True,
            'active_view': 0,
            'roi-prop': {'target_name': '', 'interleave': 0, 'target_scale': 2, 'shapes': []},
            'views': [
                {'name': 'Levels', 'state': {'layout': 0, 'roi-shown': True, 'scopes': [wv]}},
                {'name': 'Histogram', 'state': {'layout': 0, 'roi-shown': False,
                                                'scopes': [scope('histogram', 'HIS')]}},
            ],
        }],
    }


class DockToolsTest(obstest.OBSTest):
    'Pause, views and snapshot hotkeys'

    def setUp(self, config_name='saved-config', run=True):
        super().setUp(run=False, config_name=config_name)
        path = self.obs.config.get_scenecollection_file()
        if not os.path.exists(path) and os.path.exists(path + '.json'):
            path += '.json'
        with open(path, encoding='utf-8') as fr:
            collection = json.load(fr)
        collection.setdefault('modules', {})[DOCK_KEY] = dock_data()
        with open(path, 'w', encoding='utf-8') as fw:
            json.dump(collection, fw)
        self.obs.run()

    def screenshot(self, cl, name, size=(320, 256), repeat=30):
        'Render a source repeatedly, then return the last image.'
        from PIL import Image  # pylint: disable=import-outside-toplevel
        req = {'sourceName': name, 'imageFormat': 'png', 'imageWidth': size[0], 'imageHeight': size[1]}
        data = None
        for _ in range(repeat):
            try:
                data = cl.send('GetSourceScreenshot', req).image_data
            except Exception:  # pylint: disable=broad-except
                data = None
            time.sleep(0.05)
        self.assertTrue(data, f'no screenshot of {name}')
        img = Image.open(io.BytesIO(base64.b64decode(data.split(',', 1)[1]))).convert('RGB')
        os.makedirs('screenshots', exist_ok=True)
        img.save(f'screenshots/{self.name}-{name}-{int(time.time() * 1000)}.png')
        return img

    @staticmethod
    def top_row(img):
        'The highest row with a lit trace sample.'
        for y in range(img.height):
            if any(sum(img.getpixel((x, y))) > 90 for x in range(0, img.width, 4)):
                return y
        return img.height

    def kinds(self, cl):
        'The first input of each kind.'
        out = {}
        for i in cl.send('GetInputList').inputs:
            kind = i['inputKind']
            if kind.rsplit('_v', 1)[-1].isdigit():
                kind = kind.rsplit('_v', 1)[0]
            out.setdefault(kind, i['inputName'])
        return out

    def hotkey(self, cl, name):
        'Press a hotkey as a Stream Deck does.'
        cl.send('TriggerHotkeyByName', {'hotkeyName': name})
        time.sleep(0.5)

    def test_hotkeys(self):
        'Pause keeps the waveform, views switch, a snapshot can be taken'
        cl = self.obs.get_obsws()
        names = [h for h in cl.send('GetHotkeyList').hotkeys if h.startswith('VideoAnalyser.')]
        notice(f'hotkeys: {len(names)}')
        for h in ('VideoAnalyser.Pause', 'VideoAnalyser.ViewNext', 'VideoAnalyser.Snapshot',
                  'VideoAnalyser.Compare', 'VideoAnalyser.View1'):
            self.assertIn(h, names)

        video = cl.send('GetVideoSettings')
        cl.send('CreateInput', {
            'sceneName': 'Scene', 'inputName': 'Level', 'inputKind': 'color_source_v3',
            'inputSettings': {'color': 0xFF404040, 'width': video.base_width, 'height': video.base_height},
        })
        wv = self.kinds(cl)[PREFIX + 'waveform_source']
        dark = self.top_row(self.screenshot(cl, wv))

        # paused: a brighter picture does not move the trace
        self.hotkey(cl, 'VideoAnalyser.Pause')
        cl.send('SetInputSettings', {'inputName': 'Level', 'inputSettings': {'color': 0xFFE0E0E0}})
        time.sleep(1.0)
        paused = self.top_row(self.screenshot(cl, wv))
        self.hotkey(cl, 'VideoAnalyser.Pause')
        time.sleep(1.0)
        live = self.top_row(self.screenshot(cl, wv))
        notice(f'waveform top row: dark {dark}, paused {paused}, live {live}')
        self.assertLess(abs(paused - dark), 4, 'the waveform moved while paused')
        self.assertLess(live, dark - 20, 'the waveform did not follow after resuming')

        # a snapshot and compare from hotkeys do not break anything
        self.hotkey(cl, 'VideoAnalyser.Snapshot')
        self.hotkey(cl, 'VideoAnalyser.Compare')
        self.hotkey(cl, 'VideoAnalyser.Compare')

        # the next view has the histogram
        self.assertNotIn(PREFIX + 'histogram_source', self.kinds(cl))
        self.hotkey(cl, 'VideoAnalyser.ViewNext')
        time.sleep(0.5)
        self.assertIn(PREFIX + 'histogram_source', self.kinds(cl), 'the view did not switch')
        self.hotkey(cl, 'VideoAnalyser.View1')
        time.sleep(0.5)
        self.assertIn(PREFIX + 'waveform_source', self.kinds(cl), 'view 1 did not come back')


if __name__ == '__main__':
    unittest.main()
