/*
 *     Generated by class-dump 3.1.1.
 *
 *     class-dump is Copyright (C) 1997-1998, 2000-2001, 2004-2006 by Steve Nygard.
 */

#import <BackRow/BRController.h>

@class BRHeaderControl, BRImageControl, BRTextControl;

@interface BRAlertController : BRController
{
    id _eventDelegate;
    SEL _eventSelector;
    BRHeaderControl *_header;
    int _type;
    BRTextControl *_primary;
    BRTextControl *_secondary;
    BRImageControl *_image;
}

+ (id)alertForError:(id)fp8;
+ (id)alertOfType:(int)fp8 titled:(id)fp12 primaryText:(id)fp16 secondaryText:(id)fp20;
- (id)initWithType:(int)fp8 titled:(id)fp12 primaryText:(id)fp16 secondaryText:(id)fp20;
- (void)dealloc;
- (BOOL)brEventAction:(id)fp8;
- (void)controlWasActivated;
- (void)wasPushed;
- (void)setTitle:(id)fp8;
- (void)setPrimaryText:(id)fp8;
- (void)setPrimaryText:(id)fp8 withAttributes:(id)fp12;
- (id)primaryText;
- (void)setSecondaryText:(id)fp8;
- (id)secondaryText;
- (void)setSecondaryText:(id)fp8 withAttributes:(id)fp12;
- (void)setEventDelegate:(id)fp8 selector:(SEL)fp12;

@end

