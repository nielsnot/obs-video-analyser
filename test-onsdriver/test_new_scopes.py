'''
The QC, Sat/Lum, line plot and info scopes, a colorized waveform and markers: each renders
without errors in a dock, and QC reports the picture it sees.
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
    'A dock with the new scopes.'
    markers = [
        {'name': 'Ref', 'kind': 0, 'level': 75.0, 'color': 0xFF40E0F0, 'style': 1, 'width': 2.0},
    ]
    vs_markers = [
        {'name': 'Skin', 'kind': 1, 'hue': 123.0, 'sat': 40.0, 'shape': 0, 'width': 2.0},
        {'name': 'Line', 'kind': 2, 'hue': 300.0, 'style': 2},
    ]
    scopes = [
        scope('qc', 'QC', {'qc_check_freeze': True, 'qc_freeze_seconds': 1.0, 'qc_min_duration': 0.2}),
        scope('satlum', 'SL'),
        scope('lineplot', 'LP'),
        scope('info', 'Info', {'text': 'Studio A'}),
        scope('waveform', 'WV', {'components': 0x20, 'colorize': True, 'markers': markers}),
        scope('vectorscope', 'VS', {'markers': vs_markers}),
    ]
    return {
        'docks': [{
            'name': 'New scopes',
            'toolbar': True,
            'active_view': 0,
            'roi-prop': {'target_name': '', 'interleave': 0, 'target_scale': 2, 'shapes': []},
            'views': [{'name': 'View 1', 'state': {'layout': 0, 'roi-shown': True, 'scopes': scopes}}],
        }],
    }


class NewScopesTest(obstest.OBSTest):
    'New scopes in a dock'

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

    def screenshot(self, cl, name, size=(320, 240), repeat=30):
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

    def test_new_scopes(self):
        'Every new scope renders, and QC sees a black picture as black'
        cl = self.obs.get_obsws()
        video = cl.send('GetVideoSettings')
        # a grey left half and black elsewhere: borders and levels to report
        res = cl.send('CreateInput', {
            'sceneName': 'Scene', 'inputName': 'Grey', 'inputKind': 'color_source_v3',
            'inputSettings': {'color': 0xFF808080, 'width': video.base_width // 2, 'height': video.base_height},
        })
        self.assertTrue(res.scene_item_id)

        inputs = cl.send('GetInputList').inputs
        kinds = {}
        for i in inputs:
            kind = i['inputKind']
            if kind.rsplit('_v', 1)[-1].isdigit():
                kind = kind.rsplit('_v', 1)[0]
            kinds.setdefault(kind, i['inputName'])
        for kind in ('qc_source', 'satlum_source', 'lineplot_source', 'info_source', 'waveform_source',
                     'vectorscope_source'):
            self.assertIn(PREFIX + kind, kinds, f'{kind} was not created')

        for kind in ('satlum_source', 'lineplot_source', 'info_source', 'waveform_source', 'vectorscope_source'):
            img = self.screenshot(cl, kinds[PREFIX + kind])
            lit = sum(1 for x in range(0, img.width, 4) for y in range(0, img.height, 4)
                      if sum(img.getpixel((x, y))) > 60)
            notice(f'{kind}: {lit} lit samples')
            self.assertGreater(lit, 0, f'{kind} draws nothing')

        img = self.screenshot(cl, kinds[PREFIX + 'qc_source'], size=(480, 400), repeat=60)
        # the status at the top right: green when all is fine, red when a check fails
        status = [img.getpixel((x, y)) for x in range(380, 476) for y in range(4, 24)]
        red = sum(1 for r, g, b in status if r > 180 and g < 120)
        green = sum(1 for r, g, b in status if g > 160 and r < 160)
        notice(f'qc status pixels: red {red} green {green}')
        # the right half is black, so a black border is reported
        self.assertGreater(red, 0, 'QC does not report the black border')


if __name__ == '__main__':
    unittest.main()
