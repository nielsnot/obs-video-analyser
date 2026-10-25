'''
A 3D LUT applied to the scopes: an inverting LUT turns a white picture black for false color.
'''

import os
import unittest
import test_hlg
from test_hlg import HLGTestBase, near, notice

BLACK_CLIP = (217, 56, 255)  # Exposure preset, below 2 %


def write_invert_lut(path, size=5):
    'A .cube LUT that inverts R, G and B.'
    with open(path, 'w', encoding='utf-8') as fw:
        fw.write('TITLE "invert"\nLUT_3D_SIZE %d\n' % size)
        for b in range(size):
            for g in range(size):
                for r in range(size):
                    fw.write('%.4f %.4f %.4f\n' % (1 - r / (size - 1), 1 - g / (size - 1), 1 - b / (size - 1)))


class LUTTest(HLGTestBase):
    'LUT on the scopes'

    def test_lut(self):
        'White clips without the LUT and is black through an inverting LUT'
        cl = self.obs.get_obsws()
        self.add_white(cl)
        color = self.measure(cl, 'no LUT')
        self.assertTrue(near(color, test_hlg.CLIP), f'white is not at 100 %: {color}')

        path = os.path.abspath('screenshots/invert.cube')
        os.makedirs('screenshots', exist_ok=True)
        write_invert_lut(path)
        roi = self.source_of(cl, 'roi')
        cl.send('SetInputSettings', {'inputName': roi, 'inputSettings': {'lut_mode': 1, 'lut_file': path}})
        color = self.measure(cl, 'LUT on the view only')
        self.assertTrue(near(color, test_hlg.CLIP), f'a view-only LUT changed the scopes: {color}')

        cl.send('SetInputSettings', {'inputName': roi, 'inputSettings': {'lut_mode': 2}})
        color = self.measure(cl, 'LUT on the scopes')
        notice(f'through the inverting LUT: {color}')
        self.assertTrue(near(color, BLACK_CLIP), f'the LUT is not applied to the scopes: {color}')


if __name__ == '__main__':
    unittest.main()
