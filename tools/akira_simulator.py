import pygame
import sys
import os

# Paths
BG_IMAGE = os.path.join(os.path.dirname(__file__), '../docs/AkiraSim.png')

# Display region (adjust as needed)
LCD_X, LCD_Y = 90, 62
LCD_W, LCD_H = 240, 320

# Button positions (approximate, tune as needed)
BUTTONS = [
    {'name': 'PWR', 'pos': (340, 90), 'key': pygame.K_ESCAPE},
    {'name': 'SET', 'pos': (60, 90), 'key': pygame.K_RETURN},
    {'name': 'UP', 'pos': (90, 420), 'key': pygame.K_w},
    {'name': 'DOWN', 'pos': (90, 500), 'key': pygame.K_s},
    {'name': 'LEFT', 'pos': (55, 460), 'key': pygame.K_a},
    {'name': 'RIGHT', 'pos': (125, 460), 'key': pygame.K_d},
    {'name': 'X', 'pos': (310, 420), 'key': pygame.K_i},
    {'name': 'B', 'pos': (310, 500), 'key': pygame.K_k},
    {'name': 'Y', 'pos': (275, 460), 'key': pygame.K_j},
    {'name': 'A', 'pos': (345, 460), 'key': pygame.K_l},
]

pygame.init()
window = pygame.display.set_mode((400, 600))
pygame.display.set_caption('Akira Python Simulator')

# Load background
bg = pygame.image.load(BG_IMAGE).convert_alpha()
bg = pygame.transform.smoothscale(bg, (400, 600))

# LCD surface
lcd = pygame.Surface((LCD_W, LCD_H))
lcd.fill((30, 30, 30))
font = pygame.font.SysFont('Arial', 32, bold=True)
text = font.render('AKIRA SIM', True, (220, 220, 220))
lcd.blit(text, (LCD_W//2 - text.get_width()//2, LCD_H//2 - text.get_height()//2))

# Button state
button_pressed = [False] * len(BUTTONS)

clock = pygame.time.Clock()
running = True
while running:
    for event in pygame.event.get():
        if event.type == pygame.QUIT:
            running = False
        elif event.type == pygame.KEYDOWN or event.type == pygame.KEYUP:
            for i, btn in enumerate(BUTTONS):
                if event.key == btn['key']:
                    button_pressed[i] = event.type == pygame.KEYDOWN
        elif event.type == pygame.MOUSEBUTTONDOWN or event.type == pygame.MOUSEBUTTONUP:
            mx, my = pygame.mouse.get_pos()
            for i, btn in enumerate(BUTTONS):
                bx, by = btn['pos']
                if (mx-bx)**2 + (my-by)**2 < 25**2:
                    button_pressed[i] = event.type == pygame.MOUSEBUTTONDOWN
    
    window.blit(bg, (0, 0))
    window.blit(lcd, (LCD_X, LCD_Y))
    # Draw button overlays
    for i, btn in enumerate(BUTTONS):
        color = (255, 180, 40) if button_pressed[i] else (220, 220, 220)
        pygame.draw.circle(window, color, btn['pos'], 25, 0)
        label = font.render(btn['name'], True, (40, 40, 40))
        window.blit(label, (btn['pos'][0]-label.get_width()//2, btn['pos'][1]-label.get_height()//2))
    pygame.display.flip()
    clock.tick(60)
pygame.quit()
