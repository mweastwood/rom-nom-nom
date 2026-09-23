from splat.segtypes.common.cpp import CommonSegCpp


class N64SegCc(CommonSegCpp):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.file_extension = "cc"
