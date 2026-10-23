'''
Check that the dock's selection limits what the scopes analyze.

The program shows red on the left half and blue on the right half. A dock is loaded with a
vectorscope and a false color scope. Without shapes the vectorscope shows both colors; with an
ellipse around the red half it must show red only, and false color must be dark outside the ellipse.
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

# ellipse inside the left (red) half, normalized to the picture
ELLIPSE = {'type': 1, 'x0': 0.05, 'y0': 0.1, 'x1': 0.45, 'y1': 0.9}


def notice(msg):
    'Show a line in the GitHub Actions summary.'
    print(f'::notice::{msg}', flush=True)


def dock_data():
    'A dock with one view holding a vectorscope and a false color scope.'
    scopes = [
        {
            'type': 'vectorscope',
            'label': 'VS', 'show_label': False, 'use_roi': True, 'target': '',
            'prop': {'graticule': 0, 'ring_scale': False, 'hue_ring': False,
                     'intensity': 255, 'color_type': 0, 'target_scale': 2},
        },
        {
            'type': 'falsecolor',
            'label': 'FC', 'show_label': False, 'use_roi': True, 'target': '',
            'prop': {'fc_preset': 0, 'show_key': 0, 'target_scale': 2},
        },
    ]
    return {
        'docks': [{
            'name': 'Selection test',
            'toolbar': True,
            'active_view': 0,
            'roi-prop': {'target_name': '', 'interleave': 0, 'target_scale': 2, 'shapes': []},
            'views': [{'name': 'View 1', 'state': {'layout': 0, 'roi-shown': True, 'scopes': scopes}}],
        }],
    }


class SelectionTest(obstest.OBSTest):
    'Scopes in a dock follow the selection'

    def setUp(self, config_name='saved-config', run=True):
        super().setUp(run=False, config_name=config_name)
        path = self.obs.config.get_scenecollection_file()
        if not os.path.exists(path) and os.path.exists(path + '.json'):
            path += '.json'
        with open(path, encoding='utf-8') as fr:
            collection = json.load(fr)
        collection[DOCK_KEY] = dock_data()
        with open(path, 'w', encoding='utf-8') as fw:
            json.dump(collection, fw)
        self.obs.run()

    def screenshot(self, cl, name, width=None, height=None, repeat=40):
        'Render the source repeatedly so its pipeline has data, then return the last image.'
        from PIL import Image  # pylint: disable=import-outside-toplevel
        req = {'sourceName': name, 'imageFormat': 'png'}
        if width:
            req['imageWidth'] = width
            req['imageHeight'] = height
        data = None
        for _ in range(repeat):
            data = cl.send('GetSourceScreenshot', req).image_data
            time.sleep(0.05)
        raw = base64.b64decode(data.split(',', 1)[1])
        img = Image.open(io.BytesIO(raw)).convert('RGB')
        os.makedirs('screenshots', exist_ok=True)
        img.save(f'screenshots/{self.name}-{name}-{int(time.time() * 1000)}.png')
        return img

    @staticmethod
    def area_level(img, cx, cy, r):
        'Brightest value in a square around (cx, cy).'
        best = 0
        for y in range(max(cy - r, 0), min(cy + r, img.height)):
            for x in range(max(cx - r, 0), min(cx + r, img.width)):
                best = max(best, sum(img.getpixel((x, y))))
        return best

    def test_selection(self):
        'Vectorscope and false color follow the selection'
        cl = self.obs.get_obsws()

        video = cl.send('GetVideoSettings')
        w, h = video.base_width, video.base_height
        scene = 'Scene'
        for name, color, x in (('Red', 0xFF0000FF, 0), ('Blue', 0xFFFF0000, w // 2)):
            res = cl.send('CreateInput', {
                'inputName': name, 'sceneName': scene, 'inputKind': 'color_source_v3',
                'inputSettings': {'color': color, 'width': w // 2, 'height': h},
            })
            cl.send('SetSceneItemTransform', {
                'sceneName': scene, 'sceneItemId': res.scene_item_id,
                'sceneItemTransform': {'positionX': x, 'positionY': 0},
            })

        inputs = cl.send('GetInputList').inputs
        by_kind = {i['inputKind']: i['inputName'] for i in inputs}
        notice('inputs: ' + ', '.join(f"{i['inputName']} ({i['inputKind']})" for i in inputs))
        vs = by_kind.get(PREFIX + 'vectorscope_source')
        fc = by_kind.get(PREFIX + 'falsecolor_source')
        roi = by_kind.get(PREFIX + 'roi')
        self.assertTrue(vs and fc and roi, 'dock scopes were not created')

        # red sits near U 89-101, V 239 (row 16); blue near U 238, V 109-117 (rows 138-146)
        red_at, blue_at = (95, 16), (238, 142)

        # whole picture: both colors
        img = self.screenshot(cl, vs, 256, 256)
        red_all, blue_all = self.area_level(img, *red_at, 16), self.area_level(img, *blue_at, 16)
        fc_img = self.screenshot(cl, fc)
        corner_all = sum(fc_img.getpixel((2, 2)))
        notice(f'no selection: vectorscope red={red_all} blue={blue_all}; false color corner={corner_all} '
               f'size={fc_img.size}')
        self.assertGreater(red_all, 100, 'red missing from the vectorscope')
        self.assertGreater(blue_all, 100, 'blue missing from the vectorscope')

        # ellipse around the red half
        cl.send('SetInputSettings', {'inputName': roi, 'inputSettings': {'shapes': [ELLIPSE]}})
        img = self.screenshot(cl, vs, 256, 256)
        red_sel, blue_sel = self.area_level(img, *red_at, 16), self.area_level(img, *blue_at, 16)
        fc_img = self.screenshot(cl, fc)
        fw, fh = fc_img.size
        corner = sum(fc_img.getpixel((2, 2)))
        center = sum(fc_img.getpixel((fw // 2, fh // 2)))
        notice(f'ellipse: vectorscope red={red_sel} blue={blue_sel}; false color corner={corner} '
               f'center={center} size={fc_img.size}')
        self.assertGreater(red_sel, 100, 'red missing from the vectorscope with a selection')
        self.assertLess(blue_sel, 30, 'the vectorscope shows blue from outside the selection')
        self.assertLess(corner, 3 * 40, 'false color is not dark outside the selection')
        self.assertGreater(center, 3 * 50, 'false color is dark inside the selection')


if __name__ == '__main__':
    unittest.main()
